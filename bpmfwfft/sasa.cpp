#include <cstdlib>
#include <cstdio>
#include <cmath>

#include "sasa.h"
#include "vectorize.h"
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif


/**
 * Calculate the accessible surface area of each atom in a single snapshot
 *
 * Parameters
 * ----------
 * frame : 2d array, shape=[n_atoms, 3]
 *     The coordinates of the nuclei
 * n_atoms : int
 *     the major axis length of frame
 * atom_radii : 1d array, shape=[n_atoms]
 *     the van der waals radii of the atoms PLUS the probe radius
 * sphere_points : 2d array, shape=[n_sphere_points, 3]
 *     a bunch of uniformly distributed points on a sphere
 * n_sphere_points : int
 *    the number of sphere points
 * atom_selection_mask : 1d array, shape[n_atoms]
 *    one index per atom indicating whether the SASA
 *    should be computed for this atom (`atom_selection_mask[i] = 1'
 *    or or not (`atom_selection_mask[i] = 0`)
 * centered_sphere_points : WORK BUFFER 2d array, shape=[n_sphere_points, 3]
 *    empty memory that intermediate calculations can be stored in
 * neighbor_indices : WORK BUFFER 2d array, shape=[n_atoms]
 *    empty memory that intermediate calculations can be stored in
 * NOTE: the point of these work buffers is that if we want to call
 *    this function repreatedly, its more efficient not to keep re-mallocing
 *    these work buffers, but instead just reuse them.
 *
 * areas : 1d array, shape=[n_atoms]
 *     the output buffer to place the results in -- the surface area of each
 *     atom
 */
void asa_frame(const float* frame, const int n_atoms, const float* atom_radii,
               const float* sphere_points, const int n_sphere_points,
               int* neighbor_indices, float* centered_sphere_points,
               const int* atom_selection_mask, float* out_grid,
               const int* counts, const float grid_spacing)
{
    // Calculate total number of grid points
    int total_grid_points = counts[0] * counts[1] * counts[2];

    // Initialize the output grid to zero
    for (int i = 0; i < total_grid_points; i++) {
        out_grid[i] = 0.0f;
    }

    float constant = 4.0f * M_PI / n_sphere_points;

    for (int i = 0; i < n_atoms; i++) {
        // Skip atom if not in selection
        int in_selection = atom_selection_mask[i];
        if (in_selection == 0)
            continue;

        float atom_radius_i = atom_radii[i];
        fvec4 r_i(frame[i*3], frame[i*3+1], frame[i*3+2], 0);

        // Get all the atoms close to atom `i`
        int n_neighbor_indices = 0;
        for (int j = 0; j < n_atoms; j++) {
            if (i == j)
                continue;

            fvec4 r_j(frame[j*3], frame[j*3+1], frame[j*3+2], 0);
            fvec4 r_ij = r_i-r_j;
            float atom_radius_j = atom_radii[j];

            // Look for atoms `j` that are nearby atom `i`
            float radius_cutoff = atom_radius_i+atom_radius_j;
            float radius_cutoff2 = radius_cutoff*radius_cutoff;
            float r2 = dot3(r_ij, r_ij);
            if (r2 < radius_cutoff2) {
                neighbor_indices[n_neighbor_indices]  = j;
                n_neighbor_indices++;
            }
            if (r2 < 1e-10f) {
                printf("ERROR: THIS CODE IS KNOWN TO FAIL WHEN ATOMS ARE VIRTUALLY");
                printf("ON TOP OF ONE ANOTHER. YOU SUPPLIED TWO ATOMS %f", sqrtf(r2));
                printf("APART. QUITTING NOW");
                exit(1);
            }
        }

        // Center the sphere points on atom i
        for (int j = 0; j < n_sphere_points; j++) {
            centered_sphere_points[3*j] = frame[3*i] + atom_radius_i*sphere_points[3*j];
            centered_sphere_points[3*j+1] = frame[3*i+1] + atom_radius_i*sphere_points[3*j+1];
            centered_sphere_points[3*j+2] = frame[3*i+2] + atom_radius_i*sphere_points[3*j+2];
        }

        // Check if each of these points is accessible
        int k_closest_neighbor = 0;
        for (int j = 0; j < n_sphere_points; j++) {
            bool is_accessible = true;
            fvec4 r_j(centered_sphere_points[3*j], centered_sphere_points[3*j+1], centered_sphere_points[3*j+2], 0);

            // Iterate through the sphere points by cycling through them
            // in a circle, starting with k_closest_neighbor and then wrapping
            // around
            for (int k = k_closest_neighbor; k < n_neighbor_indices + k_closest_neighbor; k++) {
                int k_prime = k % n_neighbor_indices;
                float r = atom_radii[neighbor_indices[k_prime]];

                int index = neighbor_indices[k_prime];
                fvec4 r_jk = r_j-fvec4(frame[3*index], frame[3*index+1], frame[3*index+2], 0);
                if (dot3(r_jk, r_jk) < r*r) {
                    k_closest_neighbor = k;
                    is_accessible = false;
                    break;
                }
            }

            if (is_accessible) {
                // Snap coordinates to grid
                float x = roundf(r_j[0] / grid_spacing) * grid_spacing;
                float y = roundf(r_j[1] / grid_spacing) * grid_spacing;
                float z = roundf(r_j[2] / grid_spacing) * grid_spacing;

                // Calculate grid indices
                int ix = (int)(x / grid_spacing);
                int iy = (int)(y / grid_spacing);
                int iz = (int)(z / grid_spacing);

                // Ensure indices are within bounds
                if (ix >= 0 && ix < counts[0] && iy >= 0 && iy < counts[1] && iz >= 0 && iz < counts[2]) {
                    // Calculate grid index
                    int grid_index = iz * counts[1] * counts[0] + iy * counts[0] + ix;

                    // Calculate value to add
                    float value = constant * atom_radius_i * atom_radius_i;

                    // Add value to grid
                    out_grid[grid_index] += value;
                }
            }
        }
    }
}


static void generate_sphere_points(float* sphere_points, int n_points)
{
  /*
  // Compute the coordinates of points on a sphere using the
  // Golden Section Spiral algorithm.
  //
  // Parameters
  // ----------
  // sphere_points : array, shape=(n_points, 3)
  //     Empty array of length n_points*3 -- will be filled with the points
  //     as an array in C-order. i.e. sphere_points[3*i], sphere_points[3*i+1]
  //     and sphere_points[3*i+2] are the x,y,z coordinates of the ith point
  // n_pts : int
  //     Number of points to generate on the sphere
  //
  */
  int i;
  float y, r, phi;
  float inc = M_PI * (3.0 - sqrt(5.0));
  float offset = 2.0 / n_points;

  for (i = 0; i < n_points; i++) {
    y = i * offset - 1.0 + (offset / 2.0);
    r = sqrt(1.0 - y*y);
    phi = i * inc;

    sphere_points[3*i] = cos(phi) * r;
    sphere_points[3*i+1] = y;
    sphere_points[3*i+2] = sin(phi) * r;
  }
}


void sasa(const int n_frames, const int n_atoms, const float* xyzlist,
          const float* atom_radii, const int n_sphere_points,
          const int* atom_selection_mask, float* out,
          const int* counts, const float grid_spacing)
{
    int i;

    /* work buffers that will be thread-local */
    int* wb1;
    float* wb2;

    /* generate the sphere points */
    float* sphere_points = (float*) malloc(n_sphere_points*3*sizeof(float));
    generate_sphere_points(sphere_points, n_sphere_points);

    // Calculate total number of grid points
    int total_grid_points = counts[0] * counts[1] * counts[2];

#ifdef _OPENMP
    #pragma omp parallel private(wb1, wb2)
    {
#endif

    /* malloc the work buffers for each thread */
    wb1 = (int*) malloc(n_atoms*sizeof(int));
    wb2 = (float*) malloc(3*n_sphere_points*sizeof(float));

#ifdef _OPENMP
    #pragma omp for
#endif
    for (i = 0; i < n_frames; i++) {
        asa_frame(xyzlist + i*n_atoms*3, n_atoms, atom_radii, sphere_points,
                  n_sphere_points, wb1, wb2, atom_selection_mask,
                  out + i*total_grid_points, counts, grid_spacing);
    }

    free(wb1);
    free(wb2);

#ifdef _OPENMP
    } /* close omp parallel private */
#endif

    free(sphere_points);
}