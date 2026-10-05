/**************************************************************************/
/*  landscape_spline_flow.cpp                                             */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "landscape_spline_flow.h"

#include "core/object/callable_mp.h"
#include "core/templates/sort_array.h"
#include "scene/3d/landscape_spline_3d.h"

namespace LandscapeSplineFlow {

namespace {

// How strongly bends carry the fastest water over to their outer bank: the
// speed across a cross-section is scaled by 1 - BEND_SHIFT * curvature *
// offset. 1.5 more than undoes the free vortex's lead of the inner bank, and
// leaves the outer one about a third faster in a bend twice as tight as the
// river is wide.
constexpr float BEND_SHIFT = 1.5f;
// How far, in river widths, the fastest water lags behind the bend that
// moves it over.
constexpr float BEND_LAG = 0.6f;
// Wakes: how far they reach before fading to a third, and how fast they
// spread sideways (meters squared per meter travelled; a wake w meters wide
// widens by about sqrt(4 * diffusion * distance)).
constexpr float WAKE_LENGTH = 30.0f;
constexpr float WAKE_DIFFUSION = 0.05f;
// How much slower the water directly behind an obstacle runs.
constexpr float WAKE_SLOWDOWN = 0.75f;
// The churned-up water's trail behind where it was churned up: shorter, and
// spreading faster.
constexpr float TRAIL_LENGTH = 12.0f;
constexpr float TRAIL_DIFFUSION = 0.12f;
// How far upstream, in meters, a shallow is compared against: water pouring
// from deep onto shallow ground within about this distance breaks.
constexpr float RIFFLE_REACH = 3.0f;

float smoothstep_range(float p_from, float p_to, float p_x) {
	const float x = CLAMP((p_x - p_from) / (p_to - p_from), 0.0f, 1.0f);
	return x * x * (3.0f - 2.0f * x);
}

float median_of(LocalVector<float> &p_values) {
	if (p_values.is_empty()) {
		return 0.0f;
	}
	SortArray<float> sorter;
	sorter.nth_element(0, p_values.size(), p_values.size() / 2, p_values.ptr());
	return p_values[p_values.size() / 2];
}

// p_values at fractional column p_x of a row of p_count, linearly
// interpolated, spread over the columns on either side by p_spread (0 to a
// third) - the sideways diffusion of what is carried along.
float sample_row(const float *p_values, int p_count, float p_x, float p_spread) {
	auto at = [&](float p_column) {
		const float x = CLAMP(p_column, 0.0f, (float)(p_count - 1));
		const int i = MIN((int)x, p_count - 2);
		const float t = x - i;
		return p_values[i] + (p_values[i + 1] - p_values[i]) * t;
	};
	if (p_count < 2) {
		return p_count == 1 ? p_values[0] : 0.0f;
	}
	if (p_spread <= 0.0f) {
		return at(p_x);
	}
	return at(p_x) * (1.0f - 2.0f * p_spread) + (at(p_x - 1.0f) + at(p_x + 1.0f)) * p_spread;
}

} // namespace

void solve(const Grid &p_grid, Field &r_field) {
	const int columns = p_grid.columns;
	const int rows = p_grid.rows;
	const int count = MAX(columns, 0) * MAX(rows, 0);
	r_field.columns = columns;
	r_field.rows = rows;
	r_field.current.resize(count);
	r_field.turbulence.resize(count);
	r_field.wake.resize(count);
	for (int k = 0; k < count; k++) {
		r_field.current[k] = Vector2();
		r_field.turbulence[k] = 0.0f;
		r_field.wake[k] = 0.0f;
	}
	if (columns < 2 || rows < 2 || (int)p_grid.positions.size() != count || (int)p_grid.depths.size() != count) {
		return;
	}

	auto at = [columns](int p_column, int p_row) {
		return p_row * columns + p_column;
	};
	const Vector2 *pos = p_grid.positions.ptr();

	// Wet cells, and how deep the water typically is.
	LocalVector<uint8_t> wet;
	wet.resize(count);
	LocalVector<float> wet_depths;
	for (int k = 0; k < count; k++) {
		wet[k] = p_grid.depths[k] > MIN_DEPTH ? 1 : 0;
		if (wet[k]) {
			wet_depths.push_back(p_grid.depths[k]);
		}
	}
	float reference_depth = median_of(wet_depths);
	if (!(reference_depth > MIN_DEPTH)) {
		reference_depth = 1.0f;
	}

	// The depth the discharge goes through, clamped so that a puddle or an
	// abyss does not swing the current to extremes. A row that is all but
	// dry (a spline laid over ground that rises above its own surface) is
	// opened up whole: there is no telling where the water goes there, and
	// closing it would stop the current along the whole river.
	LocalVector<float> depth;
	depth.resize(count);
	LocalVector<uint8_t> opened;
	opened.resize(count);
	for (int j = 0; j < rows; j++) {
		int wet_count = 0;
		for (int i = 0; i < columns; i++) {
			wet_count += wet[at(i, j)];
		}
		const bool open_row = wet_count < MAX(2, columns / 5);
		for (int i = 0; i < columns; i++) {
			const int k = at(i, j);
			opened[k] = open_row && !wet[k];
			if (open_row) {
				wet[k] = 1;
			}
			depth[k] = wet[k] ? CLAMP(opened[k] ? reference_depth : p_grid.depths[k], 0.1f * reference_depth, 3.0f * reference_depth) : 0.0f;
		}
	}

	// Spacing between neighbors: across (between columns i and i + 1) and
	// along (between rows j and j + 1).
	LocalVector<float> across_gap;
	LocalVector<float> along_gap;
	across_gap.resize(count);
	along_gap.resize(count);
	for (int j = 0; j < rows; j++) {
		for (int i = 0; i < columns; i++) {
			const int k = at(i, j);
			across_gap[k] = i + 1 < columns ? pos[k].distance_to(pos[at(i + 1, j)]) : 0.0f;
			along_gap[k] = j + 1 < rows ? pos[k].distance_to(pos[at(i, j + 1)]) : 0.0f;
		}
	}
	// The extent of each cell around its node, across and along.
	auto cell_width = [&](int p_column, int p_row) {
		return 0.5f * ((p_column > 0 ? across_gap[at(p_column - 1, p_row)] : 0.0f) + across_gap[at(p_column, p_row)]);
	};
	auto cell_length = [&](int p_column, int p_row) {
		return 0.5f * ((p_row > 0 ? along_gap[at(p_column, p_row - 1)] : 0.0f) + along_gap[at(p_column, p_row)]);
	};

	// How readily water passes between neighbors (the discretized
	// h * grad(phi) across the face between them): depth (harmonic mean, so
	// a dry or very shallow side throttles it) times the face's length over
	// the distance. Downstream neighbor in to_next_row, the next column's in
	// to_next_column.
	LocalVector<double> to_next_row;
	LocalVector<double> to_next_column;
	to_next_row.resize(count);
	to_next_column.resize(count);
	for (int j = 0; j < rows; j++) {
		for (int i = 0; i < columns; i++) {
			const int k = at(i, j);
			to_next_row[k] = 0.0;
			to_next_column[k] = 0.0;
			if (!wet[k]) {
				continue;
			}
			if (j + 1 < rows && wet[at(i, j + 1)]) {
				const float a = depth[k];
				const float b = depth[at(i, j + 1)];
				const float face = 0.5f * (cell_width(i, j) + cell_width(i, j + 1));
				to_next_row[k] = (2.0 * a * b / (a + b)) * face / MAX(along_gap[k], 1e-4f);
			}
			if (i + 1 < columns && wet[at(i + 1, j)]) {
				const float a = depth[k];
				const float b = depth[at(i + 1, j)];
				const float face = 0.5f * (cell_length(i, j) + cell_length(i + 1, j));
				to_next_column[k] = (2.0 * a * b / (a + b)) * face / MAX(across_gap[k], 1e-4f);
			}
		}
	}

	// Which cells the current reaches at all: those joined to either end.
	// Water cut off from both (a pool behind rocks) has nothing driving it.
	LocalVector<uint8_t> flowing;
	flowing.resize(count);
	LocalVector<int> stack;
	for (int k = 0; k < count; k++) {
		flowing[k] = 0;
	}
	for (int i = 0; i < columns; i++) {
		for (const int j : { 0, rows - 1 }) {
			const int k = at(i, j);
			if (wet[k] && !flowing[k]) {
				flowing[k] = 1;
				stack.push_back(k);
			}
		}
	}
	while (!stack.is_empty()) {
		const int k = stack[stack.size() - 1];
		stack.resize(stack.size() - 1);
		const int i = k % columns;
		const int j = k / columns;
		auto visit = [&](int p_other, double p_conductance) {
			if (p_conductance > 0.0 && !flowing[p_other]) {
				flowing[p_other] = 1;
				stack.push_back(p_other);
			}
		};
		if (i > 0) {
			visit(at(i - 1, j), to_next_column[at(i - 1, j)]);
		}
		if (i + 1 < columns) {
			visit(at(i + 1, j), to_next_column[k]);
		}
		if (j > 0) {
			visit(at(i, j - 1), to_next_row[at(i, j - 1)]);
		}
		if (j + 1 < rows) {
			visit(at(i, j + 1), to_next_row[k]);
		}
	}

	// The potential: 0 at the upstream end, rising downstream. It starts out
	// as the channel's cross-sections taken in series, as if every one were
	// level across - exact for an even channel, and already right about how
	// much any shallow, pool or obstacle holds the whole river up - so what
	// is left to relax below is only how the water finds its way around
	// things locally.
	LocalVector<double> phi;
	phi.resize(count);
	double total = 0.0;
	for (int j = 0; j < rows; j++) {
		for (int i = 0; i < columns; i++) {
			phi[at(i, j)] = total;
		}
		if (j + 1 < rows) {
			double conductance = 0.0;
			for (int i = 0; i < columns; i++) {
				conductance += to_next_row[at(i, j)];
			}
			if (conductance > 0.0) {
				total += 1.0 / conductance;
			}
		}
	}
	if (!(total > 0.0)) {
		return;
	}

	// Successive over-relaxation by lines: each row solved exactly across
	// (a tridiagonal system) against its neighbors, sweeping downstream and
	// back. The ends stay fixed.
	{
		const double omega = 1.6;
		const double tolerance = 1e-4 * total / (rows - 1);
		LocalVector<double> sub;
		LocalVector<double> diagonal;
		LocalVector<double> super;
		LocalVector<double> rhs;
		LocalVector<double> solution;
		sub.resize(columns);
		diagonal.resize(columns);
		super.resize(columns);
		rhs.resize(columns);
		solution.resize(columns);
		auto relax_row = [&](int p_row) {
			double change = 0.0;
			for (int i = 0; i < columns; i++) {
				const int k = at(i, p_row);
				if (!flowing[k]) {
					sub[i] = 0.0;
					diagonal[i] = 1.0;
					super[i] = 0.0;
					rhs[i] = phi[k];
					continue;
				}
				const double up = to_next_row[at(i, p_row - 1)];
				const double down = to_next_row[k];
				const double left = i > 0 ? to_next_column[at(i - 1, p_row)] : 0.0;
				const double right = i + 1 < columns ? to_next_column[k] : 0.0;
				diagonal[i] = up + down + left + right;
				if (diagonal[i] <= 0.0) {
					sub[i] = 0.0;
					diagonal[i] = 1.0;
					super[i] = 0.0;
					rhs[i] = phi[k];
					continue;
				}
				sub[i] = -left;
				super[i] = -right;
				rhs[i] = up * phi[at(i, p_row - 1)] + down * phi[at(i, p_row + 1)];
			}
			// Thomas' algorithm.
			for (int i = 1; i < columns; i++) {
				const double m = sub[i] / diagonal[i - 1];
				diagonal[i] -= m * super[i - 1];
				rhs[i] -= m * rhs[i - 1];
			}
			solution[columns - 1] = rhs[columns - 1] / diagonal[columns - 1];
			for (int i = columns - 2; i >= 0; i--) {
				solution[i] = (rhs[i] - super[i] * solution[i + 1]) / diagonal[i];
			}
			for (int i = 0; i < columns; i++) {
				const int k = at(i, p_row);
				if (!flowing[k]) {
					continue;
				}
				const double delta = omega * (solution[i] - phi[k]);
				phi[k] += delta;
				change = MAX(change, Math::abs(delta));
			}
			return change;
		};
		for (int iteration = 0; iteration < 300; iteration++) {
			double change = 0.0;
			for (int j = 1; j < rows - 1; j++) {
				change = MAX(change, relax_row(j));
			}
			for (int j = rows - 2; j >= 1; j--) {
				change = MAX(change, relax_row(j));
			}
			if (change < tolerance) {
				break;
			}
		}
	}

	// Each cell's frame: across (towards the next column) and along
	// (downstream, square to it), from the grid itself.
	LocalVector<Vector2> across_axis;
	LocalVector<Vector2> along_axis;
	across_axis.resize(count);
	along_axis.resize(count);
	for (int j = 0; j < rows; j++) {
		for (int i = 0; i < columns; i++) {
			const int k = at(i, j);
			Vector2 across = pos[at(MIN(i + 1, columns - 1), j)] - pos[at(MAX(i - 1, 0), j)];
			across = across.length_squared() > 1e-12f ? across.normalized() : Vector2(1, 0);
			const Vector2 forward = pos[at(i, MIN(j + 1, rows - 1))] - pos[at(i, MAX(j - 1, 0))];
			Vector2 along = Vector2(-across.y, across.x);
			if (along.dot(forward) < 0.0f) {
				along = -along;
			}
			across_axis[k] = across;
			along_axis[k] = along;
		}
	}

	// The current is the potential's gradient: from its differences towards
	// whichever neighbors it flows between, against the grid's own
	// directions there.
	LocalVector<float> speed;
	speed.resize(count);
	for (int j = 0; j < rows; j++) {
		for (int i = 0; i < columns; i++) {
			const int k = at(i, j);
			speed[k] = 0.0f;
			if (!flowing[k]) {
				continue;
			}
			const bool left = i > 0 && to_next_column[at(i - 1, j)] > 0.0 && flowing[at(i - 1, j)];
			const bool right = i + 1 < columns && to_next_column[k] > 0.0 && flowing[at(i + 1, j)];
			const bool up = j > 0 && to_next_row[at(i, j - 1)] > 0.0 && flowing[at(i, j - 1)];
			const bool down = j + 1 < rows && to_next_row[k] > 0.0 && flowing[at(i, j + 1)];

			double g_across = 0.0;
			Vector2 e_across = across_axis[k];
			if (left || right) {
				const int a = left ? at(i - 1, j) : k;
				const int b = right ? at(i + 1, j) : k;
				g_across = phi[b] - phi[a];
				e_across = pos[b] - pos[a];
			}
			double g_along = 0.0;
			Vector2 e_along = along_axis[k];
			if (up || down) {
				const int a = up ? at(i, j - 1) : k;
				const int b = down ? at(i, j + 1) : k;
				g_along = phi[b] - phi[a];
				e_along = pos[b] - pos[a];
			}
			// v . e_across = g_across and v . e_along = g_along.
			const double det = (double)e_across.x * e_along.y - (double)e_across.y * e_along.x;
			if (Math::abs(det) < 1e-12) {
				continue;
			}
			const Vector2 v((float)((g_across * e_along.y - e_across.y * g_along) / det), (float)((e_across.x * g_along - g_across * e_along.x) / det));
			r_field.current[k] = Vector2(v.dot(across_axis[k]), v.dot(along_axis[k]));
			speed[k] = r_field.current[k].length();
		}
	}

	// In multiples of the typical midstream speed.
	{
		LocalVector<float> midstream;
		const int from = columns / 4;
		const int to = columns - 1 - columns / 4;
		for (int j = 0; j < rows; j++) {
			for (int i = from; i <= to; i++) {
				if (flowing[at(i, j)] && speed[at(i, j)] > 0.0f) {
					midstream.push_back(speed[at(i, j)]);
				}
			}
		}
		const float reference = median_of(midstream);
		if (!(reference > 0.0f)) {
			for (int k = 0; k < count; k++) {
				r_field.current[k] = Vector2();
			}
			return;
		}
		for (int k = 0; k < count; k++) {
			r_field.current[k] /= reference;
			speed[k] /= reference;
		}
	}

	// Bends move the fastest water over to the outer bank.
	{
		auto center_of = [&](int p_row) {
			return (pos[at(0, p_row)] + pos[at(columns - 1, p_row)]) * 0.5f;
		};
		float lagged = 0.0f;
		for (int j = 0; j < rows; j++) {
			const Vector2 center = center_of(j);
			Vector2 across = pos[at(columns - 1, j)] - pos[at(0, j)];
			const float width = MAX(across.length(), 0.5f);
			across = across.length_squared() > 1e-12f ? across.normalized() : Vector2(1, 0);

			float curvature = 0.0f;
			float step = 0.0f;
			if (j > 0 && j + 1 < rows) {
				const Vector2 before = center - center_of(j - 1);
				const Vector2 after = center_of(j + 1) - center;
				const float before_length = before.length();
				const float after_length = after.length();
				step = 0.5f * (before_length + after_length);
				if (before_length > 1e-6f && after_length > 1e-6f && step > 1e-6f) {
					const float turn = Math::abs(Math::atan2(before.cross(after), before.dot(after)));
					// Turning towards +across puts the center of the bend, and
					// the inner bank, on that side.
					const float side = (after / after_length - before / before_length).dot(across) >= 0.0f ? 1.0f : -1.0f;
					curvature = side * turn / step;
				}
			}
			lagged += (curvature - lagged) * (1.0f - Math::exp(-step / (BEND_LAG * width)));

			if (Math::abs(lagged) < 1e-6f) {
				continue;
			}
			double before_flux = 0.0;
			double after_flux = 0.0;
			float factors[512];
			const int limit = MIN(columns, 512);
			for (int i = 0; i < limit; i++) {
				const int k = at(i, j);
				const float offset = (pos[k] - center).dot(across);
				factors[i] = CLAMP(1.0f - BEND_SHIFT * lagged * offset, 0.5f, 1.5f);
				const double flux = (double)depth[k] * cell_width(i, j) * speed[k];
				before_flux += flux;
				after_flux += flux * factors[i];
			}
			if (after_flux <= 0.0) {
				continue;
			}
			const float keep = (float)(before_flux / after_flux);
			for (int i = 0; i < limit; i++) {
				const int k = at(i, j);
				r_field.current[k] *= factors[i] * keep;
				speed[k] *= factors[i] * keep;
			}
		}
	}

	// What of the dry ground stands in the water's way: islands (rocks, piers),
	// and wherever the ground along a bank juts out past the bank's usual
	// line (a cliff, a boulder against the shore). The banks themselves only
	// line the channel, however unevenly the cells outline them, and start
	// nothing.
	LocalVector<uint8_t> obstacle;
	obstacle.resize(count);
	{
		// Dry ground joined to either side of the grid is bank.
		LocalVector<uint8_t> bank;
		bank.resize(count);
		for (int k = 0; k < count; k++) {
			bank[k] = 0;
		}
		for (int j = 0; j < rows; j++) {
			for (const int i : { 0, columns - 1 }) {
				const int k = at(i, j);
				if (!wet[k] && !bank[k]) {
					bank[k] = 1;
					stack.push_back(k);
				}
			}
		}
		while (!stack.is_empty()) {
			const int k = stack[stack.size() - 1];
			stack.resize(stack.size() - 1);
			const int i = k % columns;
			const int j = k / columns;
			for (int dj = -1; dj <= 1; dj++) {
				for (int di = -1; di <= 1; di++) {
					const int ni = i + di;
					const int nj = j + dj;
					if (ni < 0 || nj < 0 || ni >= columns || nj >= rows) {
						continue;
					}
					const int n = at(ni, nj);
					if (!wet[n] && !bank[n]) {
						bank[n] = 1;
						stack.push_back(n);
					}
				}
			}
		}
		// How far the dry ground reaches in from each side, row by row, and
		// the usual reach over the rows around (the median of about ten
		// meters of them).
		LocalVector<int> reach[2];
		reach[0].resize(rows);
		reach[1].resize(rows);
		for (int j = 0; j < rows; j++) {
			int left = 0;
			while (left < columns && !wet[at(left, j)]) {
				left++;
			}
			int right = 0;
			while (right < columns && !wet[at(columns - 1 - right, j)]) {
				right++;
			}
			reach[0][j] = left;
			reach[1][j] = right;
		}
		const float row_length = rows > 1 ? pos[at(columns / 2, 0)].distance_to(pos[at(columns / 2, rows - 1)]) / (rows - 1) : 1.0f;
		const int window = CLAMP((int)Math::ceil(5.0f / MAX(row_length, 0.01f)), 2, 64);
		LocalVector<int> usual[2];
		LocalVector<int> around;
		for (int side = 0; side < 2; side++) {
			usual[side].resize(rows);
			for (int j = 0; j < rows; j++) {
				around.clear();
				for (int n = MAX(j - window, 0); n <= MIN(j + window, rows - 1); n++) {
					around.push_back(reach[side][n]);
				}
				SortArray<int> sorter;
				sorter.nth_element(0, around.size(), around.size() / 2, around.ptr());
				usual[side][j] = around[around.size() / 2];
			}
		}
		for (int j = 0; j < rows; j++) {
			for (int i = 0; i < columns; i++) {
				const int k = at(i, j);
				if (wet[k]) {
					obstacle[k] = 0;
				} else if (!bank[k]) {
					obstacle[k] = 1;
				} else {
					// More than a cell and a half past the usual line.
					obstacle[k] = (i + 1 > usual[0][j] + 1.5f && columns - i > usual[1][j] + 1.5f) ? 1 : 0;
				}
			}
		}
	}

	// Where wakes start, and where the water piles up against something: the
	// faces of the obstacles the current leaves behind, or runs into. Shallows
	// the water pours onto from deeper water break it up.
	LocalVector<float> wake_source;
	LocalVector<float> churn_source;
	wake_source.resize(count);
	churn_source.resize(count);
	LocalVector<float> deepest;
	deepest.resize(count);
	for (int j = 0; j < rows; j++) {
		for (int i = 0; i < columns; i++) {
			const int k = at(i, j);
			wake_source[k] = 0.0f;
			churn_source[k] = 0.0f;
			const float here = wet[k] && !opened[k] ? p_grid.depths[k] : 0.0f;
			deepest[k] = here;
			if (j > 0) {
				deepest[k] = MAX(here, deepest[at(i, j - 1)] * Math::exp(-along_gap[at(i, j - 1)] / RIFFLE_REACH));
			}
			if (!flowing[k] || speed[k] <= 1e-4f) {
				continue;
			}
			// Which way the obstacle faces, from all of it within two cells:
			// wide enough that the steps the cells make of a smooth outline
			// average out, rather than each step looking like a face of its
			// own. Only cells right next to it start anything.
			Vector2 normal;
			bool touching = false;
			for (int dj = -2; dj <= 2; dj++) {
				for (int di = -2; di <= 2; di++) {
					const int ni = i + di;
					const int nj = j + dj;
					if ((di == 0 && dj == 0) || ni < 0 || nj < 0 || ni >= columns || nj >= rows || !obstacle[at(ni, nj)]) {
						continue;
					}
					touching = touching || (Math::abs(di) <= 1 && Math::abs(dj) <= 1);
					const Vector2 away = pos[k] - pos[at(ni, nj)];
					const float d2 = away.length_squared();
					if (d2 > 1e-8f) {
						normal += away / (d2 * Math::sqrt(d2));
					}
				}
			}
			if (touching && normal.length_squared() > 1e-12f) {
				normal = Vector2(normal.dot(across_axis[k]), normal.dot(along_axis[k])).normalized();
				const float align = (r_field.current[k] / speed[k]).dot(normal);
				wake_source[k] = smoothstep_range(0.35f, 0.85f, align);
				churn_source[k] = smoothstep_range(0.35f, 0.85f, -align);
			}
			// A drop measured against how deep the water usually is, so the
			// small changes in the shallows along the banks do not count.
			if (here > 0.0f && deepest[k] > here) {
				churn_source[k] = MAX(churn_source[k], smoothstep_range(0.25f, 0.6f, (deepest[k] - here) / reference_depth));
			}
		}
	}

	// Carried downstream with the current, row by row: each cell takes what
	// the cell upstream of it along the current had, spread a little
	// sideways and faded with the distance travelled.
	LocalVector<float> trail;
	trail.resize(count);
	{
		LocalVector<float> wake_row;
		LocalVector<float> trail_row;
		wake_row.resize(columns);
		trail_row.resize(columns);
		for (int j = 0; j < rows; j++) {
			if (j > 0) {
				for (int i = 0; i < columns; i++) {
					wake_row[i] = r_field.wake[at(i, j - 1)];
					trail_row[i] = trail[at(i, j - 1)];
				}
			}
			for (int i = 0; i < columns; i++) {
				const int k = at(i, j);
				float wake = wake_source[k];
				float churned = MAX(churn_source[k], wake_source[k] * 0.6f);
				if (j > 0 && flowing[k] && r_field.current[k].y > 0.05f) {
					const float step = along_gap[at(i, j - 1)];
					const float spacing = MAX(i > 0 && i + 1 < columns ? cell_width(i, j) : across_gap[at(MIN(i, columns - 2), j)], 1e-3f);
					const float x = i - r_field.current[k].x / r_field.current[k].y * step / spacing;
					const float wake_spread = CLAMP(WAKE_DIFFUSION * step / (spacing * spacing), 0.0f, 0.33f);
					const float trail_spread = CLAMP(TRAIL_DIFFUSION * step / (spacing * spacing), 0.0f, 0.33f);
					wake = MAX(wake, sample_row(wake_row.ptr(), columns, x, wake_spread) * Math::exp(-step / WAKE_LENGTH));
					churned = MAX(churned, sample_row(trail_row.ptr(), columns, x, trail_spread) * Math::exp(-step / TRAIL_LENGTH));
				}
				r_field.wake[k] = flowing[k] ? CLAMP(wake, 0.0f, 1.0f) : 0.0f;
				trail[k] = flowing[k] ? CLAMP(churned, 0.0f, 1.0f) : 0.0f;
			}
		}
	}

	// The water in a wake is held back.
	for (int k = 0; k < count; k++) {
		const float keep = 1.0f - WAKE_SLOWDOWN * r_field.wake[k];
		r_field.current[k] *= keep;
		speed[k] *= keep;
	}

	// Churned up: by obstacles and shallows (carried downstream), in wakes,
	// and along the shear between slow water and fast water beside it.
	for (int j = 0; j < rows; j++) {
		for (int i = 0; i < columns; i++) {
			const int k = at(i, j);
			if (!flowing[k]) {
				continue;
			}
			float shear = 0.0f;
			if (i > 0 && i + 1 < columns && flowing[at(i - 1, j)] && flowing[at(i + 1, j)]) {
				const float span = across_gap[at(i - 1, j)] + across_gap[k];
				shear = Math::abs(speed[at(i + 1, j)] - speed[at(i - 1, j)]) / MAX(span, 1e-3f);
			}
			r_field.turbulence[k] = CLAMP(trail[k] + 0.35f * r_field.wake[k] + 0.5f * smoothstep_range(0.15f, 0.6f, shear), 0.0f, 1.0f);
		}
	}
	// Softened, so that the cells' outline does not show.
	{
		LocalVector<float> blurred;
		blurred.resize(count);
		for (int j = 0; j < rows; j++) {
			for (int i = 0; i < columns; i++) {
				float sum = 0.0f;
				float weight = 0.0f;
				for (int dj = -1; dj <= 1; dj++) {
					for (int di = -1; di <= 1; di++) {
						const int ni = i + di;
						const int nj = j + dj;
						if (ni < 0 || nj < 0 || ni >= columns || nj >= rows) {
							continue;
						}
						const float w = (di == 0 ? 2.0f : 1.0f) * (dj == 0 ? 2.0f : 1.0f);
						sum += r_field.turbulence[at(ni, nj)] * w;
						weight += w;
					}
				}
				blurred[at(i, j)] = sum / weight;
			}
		}
		for (int k = 0; k < count; k++) {
			r_field.turbulence[k] = blurred[k];
		}
	}

	const float limit = VELOCITY_RANGE * 0.98f;
	for (int k = 0; k < count; k++) {
		if (r_field.current[k].length() > limit) {
			r_field.current[k] = r_field.current[k].normalized() * limit;
		}
	}
}

void pack_rows(const Field &p_field, float p_from_row, float p_to_row, int p_rows, Vector<uint8_t> &r_texels) {
	const int columns = p_field.columns;
	const int rows = p_field.rows;
	r_texels.resize(MAX(columns, 0) * MAX(p_rows, 0) * 4);
	if (columns <= 0 || rows <= 0 || p_rows <= 0) {
		return;
	}
	uint8_t *w = r_texels.ptrw();
	auto to_byte = [](float p_value) {
		return (uint8_t)CLAMP((int)Math::round(p_value * 255.0f), 0, 255);
	};
	for (int r = 0; r < p_rows; r++) {
		const float t = p_rows > 1 ? (float)r / (p_rows - 1) : 0.0f;
		const float row = CLAMP(p_from_row + (p_to_row - p_from_row) * t, 0.0f, (float)(rows - 1));
		const int j0 = MIN((int)row, MAX(rows - 2, 0));
		const int j1 = MIN(j0 + 1, rows - 1);
		const float f = row - j0;
		for (int i = 0; i < columns; i++) {
			const int a = j0 * columns + i;
			const int b = j1 * columns + i;
			const Vector2 current = p_field.current[a].lerp(p_field.current[b], f);
			const float turbulence = Math::lerp(p_field.turbulence[a], p_field.turbulence[b], f);
			const float wake = Math::lerp(p_field.wake[a], p_field.wake[b], f);
			uint8_t *texel = w + (r * columns + i) * 4;
			texel[0] = to_byte(0.5f + 0.5f * current.x / VELOCITY_RANGE);
			texel[1] = to_byte(0.5f + 0.5f * current.y / VELOCITY_RANGE);
			texel[2] = to_byte(turbulence);
			texel[3] = to_byte(wake);
		}
	}
}

} // namespace LandscapeSplineFlow

// LandscapeFlowAtlas

LandscapeFlowAtlas *LandscapeFlowAtlas::get_singleton() {
	if (singleton == nullptr) {
		singleton = memnew(LandscapeFlowAtlas);
	}
	return singleton;
}

void LandscapeFlowAtlas::finish() {
	if (singleton != nullptr) {
		memdelete(singleton);
		singleton = nullptr;
	}
}

LandscapeFlowAtlas::~LandscapeFlowAtlas() {
	texture.unref();
	image.unref();
}

bool LandscapeFlowAtlas::_place(Tile &p_tile) {
	const int width = p_tile.rect.size.x;
	const int height = p_tile.rect.size.y;
	// The snuggest shelf it fits on, so short tiles do not waste tall ones.
	int best = -1;
	for (uint32_t i = 0; i < shelves.size(); i++) {
		const Shelf &shelf = shelves[i];
		if (shelf.height >= height && size - shelf.used_width >= width && (best < 0 || shelf.height < shelves[best].height)) {
			best = i;
		}
	}
	if (best >= 0 && shelves[best].height <= height * 2) {
		Shelf &shelf = shelves[best];
		p_tile.rect.position = Vector2i(shelf.used_width, shelf.y);
		shelf.used_width += width;
		return true;
	}
	const int y = shelves.is_empty() ? 0 : shelves[shelves.size() - 1].y + shelves[shelves.size() - 1].height;
	if (y + height <= size && width <= size) {
		Shelf shelf;
		shelf.y = y;
		shelf.height = height;
		shelf.used_width = width;
		shelves.push_back(shelf);
		p_tile.rect.position = Vector2i(0, y);
		return true;
	}
	if (best >= 0) {
		Shelf &shelf = shelves[best];
		p_tile.rect.position = Vector2i(shelf.used_width, shelf.y);
		shelf.used_width += width;
		return true;
	}
	return false;
}

bool LandscapeFlowAtlas::_repack(int p_size) {
	size = p_size;
	shelves.clear();
	// Tallest first, which is what makes shelves pack well.
	LocalVector<uint32_t> order;
	for (const KeyValue<uint32_t, Tile> &kv : tiles) {
		order.push_back(kv.key);
	}
	struct ByHeight {
		const HashMap<uint32_t, Tile> *tiles;
		bool operator()(uint32_t p_a, uint32_t p_b) const {
			const int a = (*tiles)[p_a].rect.size.y;
			const int b = (*tiles)[p_b].rect.size.y;
			return a != b ? a > b : p_a < p_b;
		}
	};
	SortArray<uint32_t, ByHeight> sorter;
	sorter.compare.tiles = &tiles;
	sorter.sort(order.ptr(), order.size());

	HashSet<ObjectID> moved;
	for (const uint32_t id : order) {
		Tile &tile = tiles[id];
		const Vector2i was = tile.rect.position;
		if (!_place(tile)) {
			return false;
		}
		if (tile.rect.position != was) {
			moved.insert(tile.owner);
		}
	}
	image = Image::create_empty(size, size, false, Image::FORMAT_RGBA8);
	for (const KeyValue<uint32_t, Tile> &kv : tiles) {
		_write(kv.value);
	}
	resized = true;
	_queue_upload();
	_notify_moved(moved);
	return true;
}

void LandscapeFlowAtlas::_write(const Tile &p_tile) {
	if (image.is_null() || p_tile.texels.size() != p_tile.rect.size.x * p_tile.rect.size.y * 4) {
		return;
	}
	const Ref<Image> source = Image::create_from_data(p_tile.rect.size.x, p_tile.rect.size.y, false, Image::FORMAT_RGBA8, p_tile.texels);
	image->blit_rect(source, Rect2i(Vector2i(), p_tile.rect.size), p_tile.rect.position);
}

void LandscapeFlowAtlas::_queue_upload() {
	if (upload_queued) {
		return;
	}
	upload_queued = true;
	callable_mp_static(&LandscapeFlowAtlas::_upload).call_deferred();
}

void LandscapeFlowAtlas::_upload() {
	if (singleton == nullptr) {
		return;
	}
	LandscapeFlowAtlas *atlas = singleton;

	// Hand back what a shrinking world no longer needs, now that no spline
	// is in the middle of letting go of its tiles. (Still marked queued
	// meanwhile, so the repack does not queue another upload.)
	if (atlas->size > MIN_SIZE) {
		int64_t used = 0;
		for (const KeyValue<uint32_t, Tile> &kv : atlas->tiles) {
			used += (int64_t)kv.value.rect.size.x * kv.value.rect.size.y;
		}
		if (used * 8 < (int64_t)atlas->size * atlas->size) {
			const int previous = atlas->size;
			if (!atlas->_repack(previous / 2)) {
				atlas->_repack(previous);
			}
		}
	}
	atlas->upload_queued = false;

	if (atlas->image.is_null()) {
		return;
	}
	if (atlas->texture.is_null()) {
		atlas->texture = ImageTexture::create_from_image(atlas->image);
	} else if (atlas->resized || atlas->texture->get_width() != atlas->size) {
		atlas->texture->set_image(atlas->image);
	} else {
		atlas->texture->update(atlas->image);
	}
	atlas->resized = false;
}

void LandscapeFlowAtlas::_notify_moved(const HashSet<ObjectID> &p_owners) {
	for (const ObjectID &owner : p_owners) {
		LandscapeSpline3D *spline = Object::cast_to<LandscapeSpline3D>(ObjectDB::get_instance(owner));
		if (spline != nullptr) {
			spline->_apply_flow_tiles();
		}
	}
}

uint32_t LandscapeFlowAtlas::allocate(ObjectID p_owner, int p_width, int p_height, const Vector<uint8_t> &p_texels) {
	ERR_FAIL_COND_V(p_width <= 0 || p_height <= 0, 0);
	ERR_FAIL_COND_V(p_texels.size() != p_width * p_height * 4, 0);
	if (p_width > MAX_SIZE || p_height > MAX_SIZE) {
		return 0;
	}

	const uint32_t id = next_id++;
	Tile &tile = tiles.insert(id, Tile())->value;
	tile.owner = p_owner;
	tile.rect = Rect2i(Vector2i(-1, -1), Vector2i(p_width, p_height));
	tile.texels = p_texels;

	if (size == 0) {
		size = MIN_SIZE;
		while (size < p_width || size < p_height) {
			size *= 2;
		}
		image = Image::create_empty(size, size, false, Image::FORMAT_RGBA8);
		resized = true;
	}
	if (_place(tiles[id])) {
		_write(tiles[id]);
		_queue_upload();
		return id;
	}
	// Full: compact what is there, then grow until it fits.
	for (int attempt = size; attempt <= MAX_SIZE; attempt *= 2) {
		if (_repack(attempt)) {
			return id;
		}
	}
	tiles.erase(id);
	int fallback = MIN_SIZE;
	while (fallback < MAX_SIZE && !_repack(fallback)) {
		fallback *= 2;
	}
	return 0;
}

bool LandscapeFlowAtlas::update(uint32_t p_tile, int p_width, int p_height, const Vector<uint8_t> &p_texels) {
	Tile *tile = tiles.getptr(p_tile);
	if (tile == nullptr || tile->rect.size != Vector2i(p_width, p_height) || p_texels.size() != p_width * p_height * 4) {
		return false;
	}
	tile->texels = p_texels;
	_write(*tile);
	_queue_upload();
	return true;
}

void LandscapeFlowAtlas::release(uint32_t p_tile) {
	if (tiles.erase(p_tile)) {
		// What was there stays in the image until something takes its place,
		// but nothing reads it any more. Shrinking waits for the upload.
		if (tiles.is_empty()) {
			shelves.clear();
		}
		_queue_upload();
	}
}

Vector4 LandscapeFlowAtlas::get_tile_transform(uint32_t p_tile) const {
	const Tile *tile = tiles.getptr(p_tile);
	if (tile == nullptr || size <= 0) {
		return Vector4();
	}
	const float inv = 1.0f / size;
	return Vector4((tile->rect.position.x + 0.5f) * inv, (tile->rect.position.y + 0.5f) * inv, (tile->rect.size.x - 1) * inv, (tile->rect.size.y - 1) * inv);
}

RID LandscapeFlowAtlas::get_texture() const {
	LandscapeFlowAtlas *self = const_cast<LandscapeFlowAtlas *>(this);
	if (self->texture.is_null()) {
		// Made now, from whatever there is, so that materials can be given its
		// RID straight away; uploads keep that RID.
		const Ref<Image> initial = image.is_valid() ? image : Image::create_empty(1, 1, false, Image::FORMAT_RGBA8);
		self->texture = ImageTexture::create_from_image(initial);
		self->resized = false;
	}
	return texture->get_rid();
}
