/*============================================================================
 *
 *     This file is part of the Code_Saturne Kernel, element of the
 *     Code_Saturne CFD tool.
 *
 *     Copyright (C) 1998-2011 EDF S.A., France
 *
 *     contact: saturne-support@edf.fr
 *
 *     The Code_Saturne Kernel is free software; you can redistribute it
 *     and/or modify it under the terms of the GNU General Public License
 *     as published by the Free Software Foundation; either version 2 of
 *     the License, or (at your option) any later version.
 *
 *     The Code_Saturne Kernel is distributed in the hope that it will be
 *     useful, but WITHOUT ANY WARRANTY; without even the implied warranty
 *     of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 *
 *     You should have received a copy of the GNU General Public License
 *     along with the Code_Saturne Kernel; if not, write to the
 *     Free Software Foundation, Inc.,
 *     51 Franklin St, Fifth Floor,
 *     Boston, MA  02110-1301  USA
 *
 *============================================================================*/

/*============================================================================
 * Sparse Matrix Representation and Operations.
 *============================================================================*/

/*
 * Notes:
 *
 * The aim of these structures and associated functions is multiple:
 *
 * - Provide an "opaque" matrix object for linear solvers, allowing possible
 *   choice of the matrix type based on run-time tuning at code initialization
 *   (depending on matrix size, architecture, and compiler, the most efficient
 *   structure for matrix.vector products may vary).
 *
 * - Provide at least a CSR matrix structure in addition to the "native"
 *   matrix structure, as this may allow us to leverage existing librairies.
 *
 * - Provide a C interface, also so as to be able to interface more easily
 *   with external libraries.
 *
 * The structures used here could easily be extended to block matrixes,
 * using for example the same structure information with 3x3 blocks which
 * could arise from coupled velocity components. This would imply that the
 * corresponding vectors be interlaced (or an interlaced copy be used
 * for recurring operations such as sparse linear system resolution),
 * for better memory locality, and possible loop unrolling.
 */

#include "cs_defs.h"

/*----------------------------------------------------------------------------
 * Standard C library headers
 *----------------------------------------------------------------------------*/

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>

#if defined(HAVE_MPI)
#include <mpi.h>
#endif

#if defined (HAVE_MKL)
#include <mkl_spblas.h>
#endif

/*----------------------------------------------------------------------------
 * BFT library headers
 *----------------------------------------------------------------------------*/

#include <bft_mem.h>
#include <bft_error.h>
#include <bft_printf.h>

/*----------------------------------------------------------------------------
 * Local headers
 *----------------------------------------------------------------------------*/

#include "cs_base.h"
#include "cs_blas.h"
#include "cs_halo.h"
#include "cs_log.h"
#include "cs_numbering.h"
#include "cs_prototypes.h"
#include "cs_perio.h"
#include "cs_timer.h"

/*----------------------------------------------------------------------------
 *  Header for the current file
 *----------------------------------------------------------------------------*/

#include "cs_matrix.h"
#include "cs_matrix_priv.h"

/*----------------------------------------------------------------------------*/

BEGIN_C_DECLS

/*=============================================================================
 * Local Macro Definitions
 *============================================================================*/

/* Variant default for Intel compiler and on Itanium (optimized by BULL)
   (Use compile flag -DNO_BULL_OPTIM to switch default to general code) */

#if (defined(__INTEL_COMPILER) && defined(__ia64__) && !defined(NO_BULL_OPTIM))
#define IA64_OPTIM
#endif

/*=============================================================================
 * Local Type Definitions
 *============================================================================*/

/* Note that most types are declared in cs_matrix_priv.h.
   only those only handled here are declared here. */

/*============================================================================
 *  Global variables
 *============================================================================*/

/* Short names for matrix types */

const char  *cs_matrix_type_name[] = {N_("native"),
                                      N_("CSR"),
                                      N_("symmetric CSR"),
                                      N_("MSR"),
                                      N_("symmetric MSR")};

/* Full names for matrix types */

const char
*cs_matrix_type_fullname[] = {N_("diagonal + faces"),
                              N_("Compressed Sparse Row"),
                              N_("symmetric Compressed Sparse Row"),
                              N_("Modified Compressed Sparse Row"),
                              N_("symmetric modified Compressed Sparse Row")};

static char _cs_glob_perio_ignore_error_str[]
  = N_("Matrix product with CS_PERIO_IGNORE rotation mode not yet\n"
       "implemented: in this case, use cs_matrix_vector_multiply_nosync\n"
       "with an external halo synchronization, preceded by a backup and\n"
       "followed by a restoration of the rotation halo.");

static char _no_exclude_diag_error_str[]
  = N_("Matrix product variant using function %s\n"
       "does not handle case with excluded diagonal.");

static const char *_matrix_operation_name[8]
  = {N_("y <- A.x"),
     N_("y <- (A-D).x"),
     N_("Symmetric y <- A.x"),
     N_("Symmetric y <- (A-D).x"),
     N_("Block y <- A.x"),
     N_("Block y <- (A-D).x"),
     N_("Block symmetric y <- A.x"),
     N_("Block symmetric y <- (A-D).x")};

cs_matrix_t            *cs_glob_matrix_default = NULL;
cs_matrix_structure_t  *cs_glob_matrix_default_struct = NULL;

/*============================================================================
 * Private function definitions
 *============================================================================*/

/*----------------------------------------------------------------------------
 * Compute matrix-vector product for one dense block: y[i] = a[i].x[i]
 *
 * Vectors and blocks may be larger than their useful size, to
 * improve data alignment.
 *
 * parameters:
 *   b_id   <-- block id
 *   b_size <-- block size, including padding:
 *              b_size[0]: useful block size
 *              b_size[1]: vector block extents
 *              b_size[2]: matrix line extents
 *              b_size[3]: matrix line*column (block) extents
 *   a      <-- Pointer to block matrixes array (usually matrix diagonal)
 *   x      <-- Multipliying vector values
 *   y      --> Resulting vector
 *----------------------------------------------------------------------------*/

static inline void
_dense_b_ax(cs_lnum_t         b_id,
            const int         b_size[4],
            const cs_real_t  *restrict a,
            const cs_real_t  *restrict x,
            cs_real_t        *restrict y)
{
  cs_lnum_t   ii, jj;

# if defined(__xlc__) /* Tell IBM compiler not to alias */
# pragma disjoint(*x, *y, * a)
# endif

  for (ii = 0; ii < b_size[0]; ii++) {
    y[b_id*b_size[1] + ii] = 0;
    for (jj = 0; jj < b_size[0]; jj++)
      y[b_id*b_size[1] + ii]
        +=   a[b_id*b_size[3] + ii*b_size[2] + jj]
           * x[b_id*b_size[1] + jj];
  }
}

/*----------------------------------------------------------------------------
 * Compute matrix-vector product for one dense block: y[i] = a[i].x[i]
 *
 * This variant uses a fixed 3x3 block, for better compiler optimization.
 *
 * parameters:
 *   b_id   <-- block id
 *   a      <-- Pointer to block matrixes array (usually matrix diagonal)
 *   x      <-- Multipliying vector values
 *   y      --> Resulting vector
 *----------------------------------------------------------------------------*/

static inline void
_dense_3_3_ax(cs_lnum_t         b_id,
              const cs_real_t  *restrict a,
              const cs_real_t  *restrict x,
              cs_real_t        *restrict y)
{
  cs_lnum_t   ii, jj;

# if defined(__xlc__) /* Tell IBM compiler not to alias */
# pragma disjoint(*x, *y, * a)
# endif

  for (ii = 0; ii < 3; ii++) {
    y[b_id*3 + ii] = 0;
    for (jj = 0; jj < 3; jj++)
      y[b_id*3 + ii] +=   a[b_id*9 + ii*3 + jj] * x[b_id*3 + jj];
  }
}

/*----------------------------------------------------------------------------
 * y[i] = da[i].x[i], with da possibly NULL
 *
 * parameters:
 *   da     <-- Pointer to coefficients array (usually matrix diagonal)
 *   x      <-- Multipliying vector values
 *   y      --> Resulting vector
 *   n_elts <-- Array size
 *----------------------------------------------------------------------------*/

static inline void
_diag_vec_p_l(const cs_real_t  *restrict da,
              const cs_real_t  *restrict x,
              cs_real_t        *restrict y,
              cs_lnum_t         n_elts)
{
  cs_lnum_t  ii;

# if defined(__xlc__) /* Tell IBM compiler not to alias */
# pragma disjoint(*x, *y, *da)
# endif

  if (da != NULL) {
#   pragma omp parallel for
    for (ii = 0; ii < n_elts; ii++)
      y[ii] = da[ii] * x[ii];
  }
  else {
#   pragma omp parallel for
    for (ii = 0; ii < n_elts; ii++)
      y[ii] = 0.0;
  }

}

/*----------------------------------------------------------------------------
 * Block version of y[i] = da[i].x[i], with da possibly NULL
 *
 * parameters:
 *   da     <-- Pointer to coefficients array (usually matrix diagonal)
 *   x      <-- Multipliying vector values
 *   y      --> Resulting vector
 *   n_elts <-- Array size
 *   b_size <-- block size, including padding:
 *              b_size[0]: useful block size
 *              b_size[1]: vector block extents
 *              b_size[2]: matrix line extents
 *              b_size[3]: matrix line*column (block) extents
 *----------------------------------------------------------------------------*/

static inline void
_b_diag_vec_p_l(const cs_real_t  *restrict da,
                const cs_real_t  *restrict x,
                cs_real_t        *restrict y,
                cs_lnum_t         n_elts,
                const int         b_size[4])
{
  cs_lnum_t   ii;

  if (da != NULL) {
#   pragma omp parallel for
    for (ii = 0; ii < n_elts; ii++)
      _dense_b_ax(ii, b_size, da, x, y);
  }
  else {
#   pragma omp parallel for
    for (ii = 0; ii < n_elts*b_size[1]; ii++)
      y[ii] = 0.0;
  }
}

/*----------------------------------------------------------------------------
 * Block version of y[i] = da[i].x[i], with da possibly NULL
 *
 * This variant uses a fixed 3x3 block, for better compiler optimization.
 *
 * parameters:
 *   da     <-- Pointer to coefficients array (usually matrix diagonal)
 *   x      <-- Multipliying vector values
 *   y      --> Resulting vector
 *   n_elts <-- Array size
 *----------------------------------------------------------------------------*/

static inline void
_3_3_diag_vec_p_l(const cs_real_t  *restrict da,
                  const cs_real_t  *restrict x,
                  cs_real_t        *restrict y,
                  cs_lnum_t         n_elts)
{
  cs_lnum_t   ii;

  if (da != NULL) {
#   pragma omp parallel for
    for (ii = 0; ii < n_elts; ii++)
      _dense_3_3_ax(ii, da, x, y);
  }
  else {
#   pragma omp parallel for
    for (ii = 0; ii < n_elts*3; ii++)
      y[ii] = 0.0;
  }
}

/*----------------------------------------------------------------------------
 * Set values from y[start_id] to y[end_id] to 0.
 *
 * parameters:
 *   y        --> Resulting vector
 *   start_id <-- start id in array
 *   end_id   <-- end id in array
 *----------------------------------------------------------------------------*/

static inline void
_zero_range(cs_real_t  *restrict y,
            cs_lnum_t   start_id,
            cs_lnum_t   end_id)
{
  cs_lnum_t   ii;

# pragma omp parallel for
  for (ii = start_id; ii < end_id; ii++)
    y[ii] = 0.0;
}

/*----------------------------------------------------------------------------
 * Set values from y[start_id] to y[end_id] to 0, block version.
 *
 * parameters:
 *   y        --> resulting vector
 *   start_id <-- start id in array
 *   end_id   <-- end id in array
 *   b_size   <-- block size, including padding:
 *                b_size[0]: useful block size
 *                b_size[1]: vector block extents
 *----------------------------------------------------------------------------*/

static inline void
_b_zero_range(cs_real_t  *restrict y,
              cs_lnum_t   start_id,
              cs_lnum_t   end_id,
              const int   b_size[2])
{
  cs_lnum_t  ii;

# pragma omp parallel for
  for (ii = start_id*b_size[1]; ii < end_id*b_size[1]; ii++)
    y[ii] = 0.0;
}

/*----------------------------------------------------------------------------
 * Set values from y[start_id] to y[end_id] to 0, block version.
 *
 * parameters:
 *   y        --> resulting vector
 *   start_id <-- start id in array
 *   end_id   <-- end id in array
 *----------------------------------------------------------------------------*/

static inline void
_3_3_zero_range(cs_real_t  *restrict y,
                cs_lnum_t   start_id,
                cs_lnum_t   end_id)
{
  cs_lnum_t  ii;

# pragma omp parallel for
  for (ii = start_id*3; ii < end_id*3; ii++)
    y[ii] = 0.0;
}

/*----------------------------------------------------------------------------
 * Descend binary tree for the ordering of a cs_gnum_t (integer) array.
 *
 * parameters:
 *   number    <-> pointer to elements that should be ordered
 *   level     <-- level of the binary tree to descend
 *   n_elts    <-- number of elements in the binary tree to descend
 *----------------------------------------------------------------------------*/

inline static void
_sort_descend_tree(cs_lnum_t  number[],
                   size_t     level,
                   size_t     n_elts)
{
  size_t lv_cur;
  cs_lnum_t num_save;

  num_save = number[level];

  while (level <= (n_elts/2)) {

    lv_cur = (2*level) + 1;

    if (lv_cur < n_elts - 1)
      if (number[lv_cur+1] > number[lv_cur]) lv_cur++;

    if (lv_cur >= n_elts) break;

    if (num_save >= number[lv_cur]) break;

    number[level] = number[lv_cur];
    level = lv_cur;

  }

  number[level] = num_save;
}

/*----------------------------------------------------------------------------
 * Order an array of global numbers.
 *
 * parameters:
 *   number   <-> number of arrays to sort
 *   n_elts   <-- number of elements considered
 *----------------------------------------------------------------------------*/

static void
_sort_local(cs_lnum_t  number[],
            size_t     n_elts)
{
  size_t i, j, inc;
  cs_lnum_t num_save;

  if (n_elts < 2)
    return;

  /* Use shell sort for short arrays */

  if (n_elts < 20) {

    /* Compute increment */
    for (inc = 1; inc <= n_elts/9; inc = 3*inc+1);

    /* Sort array */
    while (inc > 0) {
      for (i = inc; i < n_elts; i++) {
        num_save = number[i];
        j = i;
        while (j >= inc && number[j-inc] > num_save) {
          number[j] = number[j-inc];
          j -= inc;
        }
        number[j] = num_save;
      }
      inc = inc / 3;
    }

  }

  else {

    /* Create binary tree */

    i = (n_elts / 2);
    do {
      i--;
      _sort_descend_tree(number, i, n_elts);
    } while (i > 0);

    /* Sort binary tree */

    for (i = n_elts - 1 ; i > 0 ; i--) {
      num_save   = number[0];
      number[0] = number[i];
      number[i] = num_save;
      _sort_descend_tree(number, 0, i);
    }
  }
}

/*----------------------------------------------------------------------------
 * Create native matrix structure.
 *
 * Note that the structure created maps to the given existing
 * face -> cell connectivity array, so it must be destroyed before this
 * array (usually the code's main face -> cell structure) is freed.
 *
 * parameters:
 *   n_cells     <-- Local number of participating cells
 *   n_cells_ext <-- Local number of cells + ghost cells sharing a face
 *   n_faces     <-- Local number of faces
 *   face_cell   <-- Face -> cells connectivity (1 to n)
 *
 * returns:
 *   pointer to allocated native matrix structure.
 *----------------------------------------------------------------------------*/

static cs_matrix_struct_native_t *
_create_struct_native(int               n_cells,
                      int               n_cells_ext,
                      int               n_faces,
                      const cs_lnum_t  *face_cell)
{
  cs_matrix_struct_native_t  *ms;

  /* Allocate and map */

  BFT_MALLOC(ms, 1, cs_matrix_struct_native_t);

  /* Allocate and map */

  ms->n_cells = n_cells;
  ms->n_cells_ext = n_cells_ext;
  ms->n_faces = n_faces;

  ms->face_cell = face_cell;

  return ms;
}

/*----------------------------------------------------------------------------
 * Destroy native matrix structure.
 *
 * parameters:
 *   matrix  <->  Pointer to native matrix structure pointer
 *----------------------------------------------------------------------------*/

static void
_destroy_struct_native(cs_matrix_struct_native_t  **matrix)
{
  if (matrix != NULL && *matrix !=NULL) {

    BFT_FREE(*matrix);

  }
}

/*----------------------------------------------------------------------------
 * Create native matrix coefficients.
 *
 * returns:
 *   pointer to allocated native coefficients structure.
 *----------------------------------------------------------------------------*/

static cs_matrix_coeff_native_t *
_create_coeff_native(void)
{
  cs_matrix_coeff_native_t  *mc;

  /* Allocate */

  BFT_MALLOC(mc, 1, cs_matrix_coeff_native_t);

  /* Initialize */

  mc->symmetric = false;
  mc->max_block_size = 0;

  mc->da = NULL;
  mc->xa = NULL;

  mc->_da = NULL;
  mc->_xa = NULL;

  return mc;
}

/*----------------------------------------------------------------------------
 * Destroy native matrix coefficients.
 *
 * parameters:
 *   coeff  <->  Pointer to native matrix coefficients pointer
 *----------------------------------------------------------------------------*/

static void
_destroy_coeff_native(cs_matrix_coeff_native_t **coeff)
{
  if (coeff != NULL && *coeff !=NULL) {

    cs_matrix_coeff_native_t  *mc = *coeff;

    if (mc->_xa != NULL)
      BFT_FREE(mc->_xa);

    if (mc->_da != NULL)
      BFT_FREE(mc->_da);

    BFT_FREE(*coeff);

  }
}

/*----------------------------------------------------------------------------
 * Set Native matrix coefficients.
 *
 * Depending on current options and initialization, values will be copied
 * or simply mapped.
 *
 * parameters:
 *   matrix           <-- Pointer to matrix structure
 *   symmetric        <-- Indicates if extradiagonal values are symmetric
 *   interleaved      <-- Indicates if matrix coefficients are interleaved
 *   copy             <-- Indicates if coefficients should be copied
 *   da               <-- Diagonal values
 *   xa               <-- Extradiagonal values
 *----------------------------------------------------------------------------*/

static void
_set_coeffs_native(cs_matrix_t      *matrix,
                   bool              symmetric,
                   bool              interleaved,
                   bool              copy,
                   const cs_real_t  *da,
                   const cs_real_t  *xa)
{
  cs_matrix_coeff_native_t  *mc = matrix->coeffs;
  const cs_matrix_struct_native_t  *ms = matrix->structure;
  cs_lnum_t ii;
  mc->symmetric = symmetric;

  /* Map or copy values */

  if (da != NULL) {

    if (copy) {
      if (mc->_da == NULL || mc->max_block_size < matrix->b_size[3]) {
        BFT_REALLOC(mc->_da, matrix->b_size[3]*ms->n_cells, cs_real_t);
        mc->max_block_size = matrix->b_size[3];
      }
      memcpy(mc->_da, da, matrix->b_size[3]*sizeof(cs_real_t) * ms->n_cells);
      mc->da = mc->_da;
    }
    else
      mc->da = da;

  }
  else {
    mc->da = NULL;
  }

  if (xa != NULL) {

    if (interleaved || symmetric == true) {

      size_t xa_n_vals = ms->n_faces;
      if (! symmetric)
        xa_n_vals *= 2;

      if (copy) {
        if (mc->_xa == NULL)
          BFT_MALLOC(mc->_xa, xa_n_vals, cs_real_t);
        memcpy(mc->_xa, xa, xa_n_vals*sizeof(cs_real_t));
        mc->xa = mc->_xa;
      }
      else
        mc->xa = xa;

    }
    else { /* !interleaved && symmetric == false */

      assert(matrix->b_size[3] == 1);

      if (mc->_xa == NULL)
        BFT_MALLOC(mc->_xa, 2*ms->n_faces, cs_real_t);

      for (ii = 0; ii < ms->n_faces; ++ii) {
        mc->_xa[2*ii] = xa[ii];
        mc->_xa[2*ii + 1] = xa[ms->n_faces + ii];
      }
      mc->xa = mc->_xa;

    }
  }
}

/*----------------------------------------------------------------------------
 * Release shared native matrix coefficients.
 *
 * parameters:
 *   matrix <-- Pointer to matrix structure
 *----------------------------------------------------------------------------*/

static void
_release_coeffs_native(cs_matrix_t  *matrix)
{
  cs_matrix_coeff_native_t  *mc = matrix->coeffs;
  if (mc !=NULL) {
    mc->da = NULL;
    mc->xa = NULL;
  }
}

/*----------------------------------------------------------------------------
 * Get diagonal of native or MSR matrix.
 *
 * parameters:
 *   matrix <-- Pointer to matrix structure
 *   da     --> Diagonal (pre-allocated, size: n_cells)
 *----------------------------------------------------------------------------*/

static void
_get_diagonal_separate(const cs_matrix_t  *matrix,
                       cs_real_t          *restrict da)
{
  cs_lnum_t  ii, jj;
  const cs_real_t *_da;
  if (matrix->type == CS_MATRIX_NATIVE) {
    const cs_matrix_coeff_native_t  *mc = matrix->coeffs;
    _da = mc->da;
  }
  else if (matrix->type == CS_MATRIX_MSR) {
    const cs_matrix_coeff_msr_t  *mc = matrix->coeffs;
    _da = mc->x_val;
  }
  else if (matrix->type == CS_MATRIX_MSR_SYM) {
    const cs_matrix_coeff_msr_sym_t  *mc = matrix->coeffs;
    _da = mc->x_val;
  }
  const cs_lnum_t  n_cells = matrix->n_cells;

  /* Unblocked version */

  if (matrix->b_size[3] == 1) {

    if (_da != NULL) {
#     pragma omp parallel for
      for (ii = 0; ii < n_cells; ii++)
        da[ii] = _da[ii];
    }
    else {
#     pragma omp parallel for
      for (ii = 0; ii < n_cells; ii++)
        da[ii] = 0.0;
    }

  }

  /* Blocked version */

  else {

    const int *b_size = matrix->b_size;

    if (_da != NULL) {
#     pragma omp parallel for private(jj)
      for (ii = 0; ii < n_cells; ii++) {
        for (jj = 0; jj < b_size[0]; jj++)
          da[ii*b_size[1] + jj] = _da[ii*b_size[3] + jj*b_size[2] + jj];
      }
    }
    else {
#     pragma omp parallel for
      for (ii = 0; ii < n_cells*b_size[1]; ii++)
        da[ii] = 0.0;
    }
  }
}

/*----------------------------------------------------------------------------
 * Local matrix.vector product y = A.x with native matrix.
 *
 * parameters:
 *   exclude_diag <-- exclude diagonal if true
 *   matrix       <-- Pointer to matrix structure
 *   x            <-- Multipliying vector values
 *   y            --> Resulting vector
 *----------------------------------------------------------------------------*/

static void
_mat_vec_p_l_native(bool                exclude_diag,
                    const cs_matrix_t  *matrix,
                    const cs_real_t    *restrict x,
                    cs_real_t          *restrict y)
{
  cs_lnum_t  ii, jj, face_id;

  const cs_matrix_struct_native_t  *ms = matrix->structure;
  const cs_matrix_coeff_native_t  *mc = matrix->coeffs;

  const cs_real_t  *restrict xa = mc->xa;

  /* Tell IBM compiler not to alias */
# if defined(__xlc__)
# pragma disjoint(*x, *y, *xa)
# endif

  /* Diagonal part of matrix.vector product */

  if (! exclude_diag) {
    _diag_vec_p_l(mc->da, x, y, ms->n_cells);
    _zero_range(y, ms->n_cells, ms->n_cells_ext);
  }
  else
    _zero_range(y, 0, ms->n_cells_ext);

  /* Note: parallel and periodic synchronization could be delayed to here */

  /* non-diagonal terms */

  if (mc->xa != NULL) {

    if (mc->symmetric) {

      const cs_lnum_t *restrict face_cel_p = ms->face_cell;

      for (face_id = 0; face_id < ms->n_faces; face_id++) {
        ii = face_cel_p[2*face_id] -1;
        jj = face_cel_p[2*face_id + 1] -1;
        y[ii] += xa[face_id] * x[jj];
        y[jj] += xa[face_id] * x[ii];
      }

    }
    else {

      const cs_lnum_t *restrict face_cel_p = ms->face_cell;

      for (face_id = 0; face_id < ms->n_faces; face_id++) {
        ii = face_cel_p[2*face_id] -1;
        jj = face_cel_p[2*face_id + 1] -1;
        y[ii] += xa[2*face_id] * x[jj];
        y[jj] += xa[2*face_id + 1] * x[ii];
      }

    }

  }
}

/*----------------------------------------------------------------------------
 * Local matrix.vector product y = A.x with native matrix.
 *
 * parameters:
 *   exclude_diag <-- exclude diagonal if true
 *   matrix       <-- Pointer to matrix structure
 *   x            <-- Multipliying vector values
 *   y            --> Resulting vector
 *----------------------------------------------------------------------------*/

static void
_b_mat_vec_p_l_native(bool                exclude_diag,
                      const cs_matrix_t  *matrix,
                      const cs_real_t    *restrict x,
                      cs_real_t          *restrict y)
{
  cs_lnum_t  ii, jj, kk, face_id;

  const cs_matrix_struct_native_t  *ms = matrix->structure;
  const cs_matrix_coeff_native_t  *mc = matrix->coeffs;

  const cs_real_t  *restrict xa = mc->xa;
  const int *b_size = matrix->b_size;

  /* Tell IBM compiler not to alias */
# if defined(__xlc__)
# pragma disjoint(*x, *y, *xa)
# endif

  /* Diagonal part of matrix.vector product */

  if (! exclude_diag) {
    _b_diag_vec_p_l(mc->da, x, y, ms->n_cells, b_size);
    _b_zero_range(y, ms->n_cells, ms->n_cells_ext, b_size);
  }
  else
    _b_zero_range(y, 0, ms->n_cells_ext, b_size);

  /* Note: parallel and periodic synchronization could be delayed to here */

  /* non-diagonal terms */

  if (mc->xa != NULL) {

    if (mc->symmetric) {

      const cs_lnum_t *restrict face_cel_p = ms->face_cell;

      for (face_id = 0; face_id < ms->n_faces; face_id++) {
        ii = face_cel_p[2*face_id] -1;
        jj = face_cel_p[2*face_id + 1] -1;
        for (kk = 0; kk < b_size[0]; kk++) {
          y[ii*b_size[1] + kk] += xa[face_id] * x[jj*b_size[1] + kk];
          y[jj*b_size[1] + kk] += xa[face_id] * x[ii*b_size[1] + kk];
        }
      }
    }
    else {

      const cs_lnum_t *restrict face_cel_p = ms->face_cell;

      for (face_id = 0; face_id < ms->n_faces; face_id++) {
        ii = face_cel_p[2*face_id] -1;
        jj = face_cel_p[2*face_id + 1] -1;
        for (kk = 0; kk < b_size[0]; kk++) {
          y[ii*b_size[1] + kk] += xa[2*face_id]     * x[jj*b_size[1] + kk];
          y[jj*b_size[1] + kk] += xa[2*face_id + 1] * x[ii*b_size[1] + kk];
        }
      }

    }

  }

}

/*----------------------------------------------------------------------------
 * Local matrix.vector product y = A.x with native matrix.
 *
 * This variant uses a fixed 3x3 block, for better compiler optimization.
 *
 * parameters:
 *   exclude_diag <-- exclude diagonal if true
 *   matrix       <-- Pointer to matrix structure
 *   x            <-- Multipliying vector values
 *   y            --> Resulting vector
 *----------------------------------------------------------------------------*/

static void
_3_3_mat_vec_p_l_native(bool                exclude_diag,
                        const cs_matrix_t  *matrix,
                        const cs_real_t    *restrict x,
                        cs_real_t          *restrict y)
{
  cs_lnum_t  ii, jj, kk, face_id;

  const cs_matrix_struct_native_t  *ms = matrix->structure;
  const cs_matrix_coeff_native_t  *mc = matrix->coeffs;

  const cs_real_t  *restrict xa = mc->xa;

  assert(matrix->b_size[0] == 3 && matrix->b_size[3] == 9);

  /* Tell IBM compiler not to alias */
# if defined(__xlc__)
# pragma disjoint(*x, *y, *xa)
# endif

  /* Diagonal part of matrix.vector product */

  if (! exclude_diag) {
    _3_3_diag_vec_p_l(mc->da, x, y, ms->n_cells);
    _3_3_zero_range(y, ms->n_cells, ms->n_cells_ext);
  }
  else
    _3_3_zero_range(y, 0, ms->n_cells_ext);

  /* Note: parallel and periodic synchronization could be delayed to here */

  /* non-diagonal terms */

  if (mc->xa != NULL) {

    if (mc->symmetric) {

      const cs_lnum_t *restrict face_cel_p = ms->face_cell;

      for (face_id = 0; face_id < ms->n_faces; face_id++) {
        ii = face_cel_p[2*face_id] -1;
        jj = face_cel_p[2*face_id + 1] -1;
        for (kk = 0; kk < 3; kk++) {
          y[ii*3 + kk] += xa[face_id] * x[jj*3 + kk];
          y[jj*3 + kk] += xa[face_id] * x[ii*3 + kk];
        }
      }
    }
    else {

      const cs_lnum_t *restrict face_cel_p = ms->face_cell;

      for (face_id = 0; face_id < ms->n_faces; face_id++) {
        ii = face_cel_p[2*face_id] -1;
        jj = face_cel_p[2*face_id + 1] -1;
        for (kk = 0; kk < 3; kk++) {
          y[ii*3 + kk] += xa[2*face_id]     * x[jj*3 + kk];
          y[jj*3 + kk] += xa[2*face_id + 1] * x[ii*3 + kk];
        }
      }

    }

  }

}

#if defined(HAVE_OPENMP) /* OpenMP variants */

/*----------------------------------------------------------------------------
 * Local matrix.vector product y = A.x with native matrix.
 *
 * parameters:
 *   exclude_diag <-- exclude diagonal if true
 *   matrix       <-- Pointer to matrix structure
 *   x            <-- Multipliying vector values
 *   y            --> Resulting vector
 *----------------------------------------------------------------------------*/

static void
_mat_vec_p_l_native_omp(bool                exclude_diag,
                        const cs_matrix_t  *matrix,
                        const cs_real_t    *restrict x,
                        cs_real_t          *restrict y)
{
  int g_id, t_id;
  cs_lnum_t  ii, jj, face_id;

  const int n_threads = matrix->numbering->n_threads;
  const int n_groups = matrix->numbering->n_groups;
  const cs_lnum_t *group_index = matrix->numbering->group_index;

  const cs_matrix_struct_native_t  *ms = matrix->structure;
  const cs_matrix_coeff_native_t  *mc = matrix->coeffs;
  const cs_real_t  *restrict xa = mc->xa;

  assert(matrix->numbering->type == CS_NUMBERING_THREADS);

  /* Tell IBM compiler not to alias */

# if defined(__xlc__)
# pragma disjoint(*x, *y, *xa)
# endif

  /* Diagonal part of matrix.vector product */

  if (! exclude_diag) {
    _diag_vec_p_l(mc->da, x, y, ms->n_cells);
    _zero_range(y, ms->n_cells, ms->n_cells_ext);
  }
  else
    _zero_range(y, 0, ms->n_cells_ext);

  /* Note: parallel and periodic synchronization could be delayed to here */

  /* non-diagonal terms */

  if (mc->xa != NULL) {

    if (mc->symmetric) {

      const cs_lnum_t *restrict face_cel_p = ms->face_cell;

      for (g_id=0; g_id < n_groups; g_id++) {

#       pragma omp parallel for private(face_id, ii, jj)
        for (t_id=0; t_id < n_threads; t_id++) {

          for (face_id = group_index[(t_id*n_groups + g_id)*2];
               face_id < group_index[(t_id*n_groups + g_id)*2 + 1];
               face_id++) {
            ii = face_cel_p[2*face_id] -1;
            jj = face_cel_p[2*face_id + 1] -1;
            y[ii] += xa[face_id] * x[jj];
            y[jj] += xa[face_id] * x[ii];
          }
        }
      }
    }
    else {

      const cs_lnum_t *restrict face_cel_p = ms->face_cell;

      for (g_id=0; g_id < n_groups; g_id++) {

#       pragma omp parallel for private(face_id, ii, jj)
        for (t_id=0; t_id < n_threads; t_id++) {

          for (face_id = group_index[(t_id*n_groups + g_id)*2];
               face_id < group_index[(t_id*n_groups + g_id)*2 + 1];
               face_id++) {
            ii = face_cel_p[2*face_id] -1;
            jj = face_cel_p[2*face_id + 1] -1;
            y[ii] += xa[2*face_id] * x[jj];
            y[jj] += xa[2*face_id + 1] * x[ii];
          }
        }
      }
    }

  }
}

/*----------------------------------------------------------------------------
 * Local matrix.vector product y = A.x with native matrix, blocked version
 *
 * parameters:
 *   exclude_diag <-- exclude diagonal if true
 *   matrix       <-- Pointer to matrix structure
 *   x            <-- Multipliying vector values
 *   y            --> Resulting vector
 *----------------------------------------------------------------------------*/

static void
_b_mat_vec_p_l_native_omp(bool                exclude_diag,
                          const cs_matrix_t  *matrix,
                          const cs_real_t    *restrict x,
                          cs_real_t          *restrict y)
{
  int g_id, t_id;
  cs_lnum_t  ii, jj, kk, face_id;
  const int *b_size = matrix->b_size;

  const int n_threads = matrix->numbering->n_threads;
  const int n_groups = matrix->numbering->n_groups;
  const cs_lnum_t *group_index = matrix->numbering->group_index;

  const cs_matrix_struct_native_t  *ms = matrix->structure;
  const cs_matrix_coeff_native_t  *mc = matrix->coeffs;
  const cs_real_t  *restrict xa = mc->xa;

  assert(matrix->numbering->type == CS_NUMBERING_THREADS);

  /* Tell IBM compiler not to alias */

# if defined(__xlc__)
# pragma disjoint(*x, *y, *xa)
# endif

  /* Diagonal part of matrix.vector product */

  if (! exclude_diag) {
    _b_diag_vec_p_l(mc->da, x, y, ms->n_cells, b_size);
    _b_zero_range(y, ms->n_cells, ms->n_cells_ext, b_size);
  }
  else
    _b_zero_range(y, 0, ms->n_cells_ext, b_size);

  /* Note: parallel and periodic synchronization could be delayed to here */

  /* non-diagonal terms */

  if (mc->xa != NULL) {

    if (mc->symmetric) {

      const cs_lnum_t *restrict face_cel_p = ms->face_cell;

      for (g_id=0; g_id < n_groups; g_id++) {

#       pragma omp parallel for private(face_id, ii, jj, kk)
        for (t_id=0; t_id < n_threads; t_id++) {

          for (face_id = group_index[(t_id*n_groups + g_id)*2];
               face_id < group_index[(t_id*n_groups + g_id)*2 + 1];
               face_id++) {
            ii = face_cel_p[2*face_id] -1;
            jj = face_cel_p[2*face_id + 1] -1;
            for (kk = 0; kk < b_size[0]; kk++) {
              y[ii*b_size[1] + kk] += xa[face_id] * x[jj*b_size[1] + kk];
              y[jj*b_size[1] + kk] += xa[face_id] * x[ii*b_size[1] + kk];
            }
          }
        }
      }

    }
    else {

      const cs_lnum_t *restrict face_cel_p = ms->face_cell;

      for (g_id=0; g_id < n_groups; g_id++) {

#       pragma omp parallel for private(face_id, ii, jj, kk)
        for (t_id=0; t_id < n_threads; t_id++) {

          for (face_id = group_index[(t_id*n_groups + g_id)*2];
               face_id < group_index[(t_id*n_groups + g_id)*2 + 1];
               face_id++) {
            ii = face_cel_p[2*face_id] -1;
            jj = face_cel_p[2*face_id + 1] -1;
            for (kk = 0; kk < b_size[0]; kk++) {
              y[ii*b_size[1] + kk] += xa[2*face_id]     * x[jj*b_size[1] + kk];
              y[jj*b_size[1] + kk] += xa[2*face_id + 1] * x[ii*b_size[1] + kk];
            }
          }
        }
      }

    }

  }
}

#endif /* defined(HAVE_OPENMP) */

static void
_mat_vec_p_l_native_bull(bool                exclude_diag,
                         const cs_matrix_t  *matrix,
                         const cs_real_t    *restrict x,
                         cs_real_t          *restrict y)
{
  cs_lnum_t  ii, ii_prev, kk, face_id, kk_max;
  cs_real_t y_it, y_it_prev;
  const cs_matrix_struct_native_t  *ms = matrix->structure;
  const cs_matrix_coeff_native_t  *mc = matrix->coeffs;
  const cs_real_t  *restrict xa = mc->xa;
  const int l1_cache_size
    = (matrix->loop_length > 0) ? matrix->loop_length : 508;

  /* Diagonal part of matrix.vector product */

  if (! exclude_diag) {
    _diag_vec_p_l(mc->da, x, y, ms->n_cells);
    //_zero_range(y, ms->n_cells, ms->n_cells_ext);
  }
  else
    _zero_range(y, 0, ms->n_cells_ext);

  for (ii = ms->n_cells; ii < ms->n_cells_ext; y[ii++] = 0.0);

  /* Note: parallel and periodic synchronization could be delayed to here */

  /* non-diagonal terms */

  if (mc->xa != NULL) {

    /*
     * 1/ Split y[ii] and y[jj] computation into 2 loops to remove compiler
     *    data dependency assertion between y[ii] and y[jj].
     * 2/ keep index (*face_cel_p) in L1 cache from y[ii] loop to y[jj] loop
     *    and xa in L2 cache.
     * 3/ break high frequency occurence of data dependency from one iteration
     *    to another in y[ii] loop (nonzero matrix value on the same line ii).
     */

    if (mc->symmetric) {

      const cs_lnum_t *restrict face_cel_p = ms->face_cell;

      for (face_id = 0;
           face_id < ms->n_faces;
           face_id += l1_cache_size) {

        kk_max = CS_MIN((ms->n_faces - face_id), l1_cache_size);

        /* sub-loop to compute y[ii] += xa[face_id] * x[jj] */

        ii = face_cel_p[0] - 1;
        ii_prev = ii;
        y_it_prev = y[ii_prev] + xa[face_id] * x[face_cel_p[1] - 1];

        for (kk = 1; kk < kk_max; ++kk) {
          ii = face_cel_p[2*kk] - 1;
          /* y[ii] += xa[face_id+kk] * x[jj]; */
          if (ii == ii_prev) {
            y_it = y_it_prev;
          }
          else {
            y_it = y[ii];
            y[ii_prev] = y_it_prev;
          }
          ii_prev = ii;
          y_it_prev = y_it + xa[face_id+kk] * x[face_cel_p[2*kk+1] - 1];
        }
        y[ii] = y_it_prev;

        /* sub-loop to compute y[ii] += xa[face_id] * x[jj] */

        for (kk = 0; kk < kk_max; ++kk) {
          y[face_cel_p[2*kk+1] - 1]
            += xa[face_id+kk] * x[face_cel_p[2*kk] - 1];
        }
        face_cel_p += 2 * l1_cache_size;
      }

    }
    else {

      const cs_lnum_t *restrict face_cel_p = ms->face_cell;

      for (face_id = 0;
           face_id < ms->n_faces;
           face_id+=l1_cache_size) {

        kk_max = CS_MIN((ms->n_faces - face_id),
                        l1_cache_size);

        /* sub-loop to compute y[ii] += xa[2*face_id] * x[jj] */

        ii = face_cel_p[0] - 1;
        ii_prev = ii;
        y_it_prev = y[ii_prev] + xa[2*face_id] * x[face_cel_p[1] - 1];

        for (kk = 1; kk < kk_max; ++kk) {
          ii = face_cel_p[2*kk] - 1;
          /* y[ii] += xa[2*(face_id+i)] * x[jj]; */
          if (ii == ii_prev) {
            y_it = y_it_prev;
          }
          else {
            y_it = y[ii];
            y[ii_prev] = y_it_prev;
          }
          ii_prev = ii;
          y_it_prev = y_it + xa[2*(face_id+kk)] * x[face_cel_p[2*kk+1] - 1];
        }
        y[ii] = y_it_prev;

        /* sub-loop to compute y[ii] += xa[2*face_id + 1] * x[jj] */

        for (kk = 0; kk < kk_max; ++kk) {
          y[face_cel_p[2*kk+1] - 1]
            += xa[2*(face_id+kk) + 1] * x[face_cel_p[2*kk] - 1];
        }
        face_cel_p += 2 * l1_cache_size;
      }

    }
  }
}

#if defined(SX) && defined(_SX) /* For vector machines */

/*----------------------------------------------------------------------------
 * Local matrix.vector product y = A.x with native matrix.
 *
 * parameters:
 *   exclude_diag <-- exclude diagonal if true
 *   matrix       <-- Pointer to matrix structure
 *   x            <-- Multipliying vector values
 *   y            --> Resulting vector
 *----------------------------------------------------------------------------*/

static void
_mat_vec_p_l_native_vector(bool                exclude_diag,
                           const cs_matrix_t  *matrix,
                           const cs_real_t    *restrict x,
                           cs_real_t          *restrict y)
{
  cs_lnum_t  ii, jj, face_id;
  const cs_matrix_struct_native_t  *ms = matrix->structure;
  const cs_matrix_coeff_native_t  *mc = matrix->coeffs;
  const cs_real_t  *restrict xa = mc->xa;

  assert(matrix->numbering->type == CS_NUMBERING_VECTORIZE);

  /* Diagonal part of matrix.vector product */

  if (! exclude_diag) {
    _diag_vec_p_l(mc->da, x, y, ms->n_cells);
    _zero_range(y, ms->n_cells, ms->n_cells_ext);
  }
  else
    _zero_range(y, 0, ms->n_cells_ext);

  /* Note: parallel and periodic synchronization could be delayed to here */

  /* non-diagonal terms */

  if (mc->xa != NULL) {

    if (mc->symmetric) {

      const cs_lnum_t *restrict face_cel_p = ms->face_cell;

#     pragma dir nodep
      for (face_id = 0; face_id < ms->n_faces; face_id++) {
        ii = face_cel_p[2*face_id] -1;
        jj = face_cel_p[2*face_id + 1] -1;
        y[ii] += xa[face_id] * x[jj];
        y[jj] += xa[face_id] * x[ii];
      }

    }
    else {

      const cs_lnum_t *restrict face_cel_p = ms->face_cell;

#     pragma dir nodep
      for (face_id = 0; face_id < ms->n_faces; face_id++) {
        ii = face_cel_p[2*face_id] -1;
        jj = face_cel_p[2*face_id + 1] -1;
        y[ii] += xa[2*face_id] * x[jj];
        y[jj] += xa[2*face_id + 1] * x[ii];
      }

    }

  }
}

#endif /* Vector machine variant */

/*----------------------------------------------------------------------------
 * Create a CSR matrix structure from a native matrix stucture.
 *
 * Note that the structure created maps global cell numbers to the given
 * existing face -> cell connectivity array, so it must be destroyed before
 * this array (usually the code's global cell numbering) is freed.
 *
 * parameters:
 *   have_diag   <-- Indicates if the diagonal is nonzero
 *   n_cells     <-- Local number of participating cells
 *   n_cells_ext <-- Local number of cells + ghost cells sharing a face
 *   n_faces     <-- Local number of faces
 *   cell_num    <-- Global cell numbers (1 to n)
 *   face_cell   <-- Face -> cells connectivity (1 to n)
 *
 * returns:
 *   pointer to allocated CSR matrix structure.
 *----------------------------------------------------------------------------*/

static cs_matrix_struct_csr_t *
_create_struct_csr(bool              have_diag,
                   int               n_cells,
                   int               n_cells_ext,
                   int               n_faces,
                   const cs_lnum_t  *face_cell)
{
  int n_cols_max;
  cs_lnum_t ii, jj, face_id;
  const cs_lnum_t *restrict face_cel_p;

  cs_lnum_t  diag_elts = 1;
  cs_lnum_t  *ccount = NULL;

  cs_matrix_struct_csr_t  *ms;

  /* Allocate and map */

  BFT_MALLOC(ms, 1, cs_matrix_struct_csr_t);

  ms->n_rows = n_cells;
  ms->n_cols = n_cells_ext;

  ms->direct_assembly = true;
  ms->have_diag = have_diag;

  BFT_MALLOC(ms->row_index, ms->n_rows + 1, cs_lnum_t);

  /* Count number of nonzero elements per row */

  BFT_MALLOC(ccount, ms->n_cols, cs_lnum_t);

  if (have_diag == false)
    diag_elts = 0;

  for (ii = 0; ii < ms->n_rows; ii++)  /* count starting with diagonal terms */
    ccount[ii] = diag_elts;

  if (face_cell != NULL) {

    face_cel_p = face_cell;

    for (face_id = 0; face_id < n_faces; face_id++) {
      ii = *face_cel_p++ - 1;
      jj = *face_cel_p++ - 1;
      ccount[ii] += 1;
      ccount[jj] += 1;
    }

  } /* if (face_cell != NULL) */

  n_cols_max = 0;

  ms->row_index[0] = 0;
  for (ii = 0; ii < ms->n_rows; ii++) {
    ms->row_index[ii+1] = ms->row_index[ii] + ccount[ii];
    if (ccount[ii] > n_cols_max)
      n_cols_max = ccount[ii];
    ccount[ii] = diag_elts; /* pre-count for diagonal terms */
  }

  ms->n_cols_max = n_cols_max;

  /* Build structure */

  BFT_MALLOC(ms->col_id, (ms->row_index[ms->n_rows]), cs_lnum_t);

  if (have_diag == true) {
    for (ii = 0; ii < ms->n_rows; ii++) {    /* diagonal terms */
      ms->col_id[ms->row_index[ii]] = ii;
    }
  }

  if (face_cell != NULL) {                   /* non-diagonal terms */

    face_cel_p = face_cell;

    for (face_id = 0; face_id < n_faces; face_id++) {
      ii = *face_cel_p++ - 1;
      jj = *face_cel_p++ - 1;
      if (ii < ms->n_rows) {
        ms->col_id[ms->row_index[ii] + ccount[ii]] = jj;
        ccount[ii] += 1;
      }
      if (jj < ms->n_rows) {
        ms->col_id[ms->row_index[jj] + ccount[jj]] = ii;
        ccount[jj] += 1;
      }
    }

  } /* if (face_cell != NULL) */

  BFT_FREE(ccount);

  /* Sort line elements by column id (for better access patterns) */

  if (n_cols_max > 1) {

    for (ii = 0; ii < ms->n_rows; ii++) {
      cs_lnum_t *col_id = ms->col_id + ms->row_index[ii];
      cs_lnum_t n_cols = ms->row_index[ii+1] - ms->row_index[ii];
      cs_lnum_t col_id_prev = -1;
      _sort_local(col_id, ms->row_index[ii+1] - ms->row_index[ii]);
      for (jj = 0; jj < n_cols; jj++) {
        if (col_id[jj] == col_id_prev)
          ms->direct_assembly = false;
        col_id_prev = col_id[jj];
      }
    }

  }

  /* Compact elements if necessary */

  if (ms->direct_assembly == false) {

    cs_lnum_t *tmp_row_index = NULL;
    cs_lnum_t  kk = 0;

    BFT_MALLOC(tmp_row_index, ms->n_rows+1, cs_lnum_t);
    memcpy(tmp_row_index, ms->row_index, (ms->n_rows+1)*sizeof(cs_lnum_t));

    kk = 0;

    for (ii = 0; ii < ms->n_rows; ii++) {
      cs_lnum_t *col_id = ms->col_id + ms->row_index[ii];
      cs_lnum_t n_cols = ms->row_index[ii+1] - ms->row_index[ii];
      cs_lnum_t col_id_prev = -1;
      ms->row_index[ii] = kk;
      for (jj = 0; jj < n_cols; jj++) {
        if (col_id_prev != col_id[jj]) {
          ms->col_id[kk++] = col_id[jj];
          col_id_prev = col_id[jj];
        }
      }
    }
    ms->row_index[ms->n_rows] = kk;

    assert(ms->row_index[ms->n_rows] < tmp_row_index[ms->n_rows]);

    BFT_FREE(tmp_row_index);
    BFT_REALLOC(ms->col_id, (ms->row_index[ms->n_rows]), cs_lnum_t);

  }

  return ms;
}

/*----------------------------------------------------------------------------
 * Destroy CSR matrix structure.
 *
 * parameters:
 *   matrix  <->  Pointer to CSR matrix structure pointer
 *----------------------------------------------------------------------------*/

static void
_destroy_struct_csr(cs_matrix_struct_csr_t  **matrix)
{
  if (matrix != NULL && *matrix !=NULL) {

    cs_matrix_struct_csr_t  *ms = *matrix;

    if (ms->row_index != NULL)
      BFT_FREE(ms->row_index);

    if (ms->col_id != NULL)
      BFT_FREE(ms->col_id);

    BFT_FREE(ms);

    *matrix = ms;

  }
}

/*----------------------------------------------------------------------------
 * Create CSR matrix coefficients.
 *
 * returns:
 *   pointer to allocated CSR coefficients structure.
 *----------------------------------------------------------------------------*/

static cs_matrix_coeff_csr_t *
_create_coeff_csr(void)
{
  cs_matrix_coeff_csr_t  *mc;

  /* Allocate */

  BFT_MALLOC(mc, 1, cs_matrix_coeff_csr_t);

  /* Initialize */

  mc->n_prefetch_rows = 0;

  mc->val = NULL;

  mc->x_prefetch = NULL;

  return mc;
}

/*----------------------------------------------------------------------------
 * Destroy CSR matrix coefficients.
 *
 * parameters:
 *   coeff  <->  Pointer to CSR matrix coefficients pointer
 *----------------------------------------------------------------------------*/

static void
_destroy_coeff_csr(cs_matrix_coeff_csr_t **coeff)
{
  if (coeff != NULL && *coeff !=NULL) {

    cs_matrix_coeff_csr_t  *mc = *coeff;

    if (mc->val != NULL)
      BFT_FREE(mc->val);

    if (mc->x_prefetch != NULL)
      BFT_FREE(mc->x_prefetch);

    BFT_FREE(*coeff);

  }
}

/*----------------------------------------------------------------------------
 * Set CSR extradiagonal matrix coefficients for the case where direct
 * assignment is possible (i.e. when there are no multiple contributions
 * to a given coefficient).
 *
 * parameters:
 *   matrix      <-- Pointer to matrix structure
 *   symmetric   <-- Indicates if extradiagonal values are symmetric
 *   interleaved <-- Indicates if matrix coefficients are interleaved
 *   xa          <-- Extradiagonal values
 *----------------------------------------------------------------------------*/

static void
_set_xa_coeffs_csr_direct(cs_matrix_t      *matrix,
                          bool              symmetric,
                          bool              interleaved,
                          const cs_real_t  *restrict xa)
{
  cs_lnum_t  ii, jj, face_id;
  cs_matrix_coeff_csr_t  *mc = matrix->coeffs;

  const cs_matrix_struct_csr_t  *ms = matrix->structure;

  /* Copy extra-diagonal values */

  assert(matrix->face_cell != NULL);

  if (symmetric == false) {

    const cs_lnum_t n_faces = matrix->n_faces;
    const cs_lnum_t *restrict face_cel_p = matrix->face_cell;

    const cs_real_t  *restrict xa1 = xa;
    const cs_real_t  *restrict xa2 = xa + matrix->n_faces;

    if (interleaved == false) {
      for (face_id = 0; face_id < n_faces; face_id++) {
        cs_lnum_t kk, ll;
        ii = *face_cel_p++ - 1;
        jj = *face_cel_p++ - 1;
        if (ii < ms->n_rows) {
          for (kk = ms->row_index[ii]; ms->col_id[kk] != jj; kk++);
          mc->val[kk] = xa1[face_id];
        }
        if (jj < ms->n_rows) {
          for (ll = ms->row_index[jj]; ms->col_id[ll] != ii; ll++);
          mc->val[ll] = xa2[face_id];
        }
      }
    }
    else { /* interleaved == true */
      for (face_id = 0; face_id < n_faces; face_id++) {
        cs_lnum_t kk, ll;
        ii = *face_cel_p++ - 1;
        jj = *face_cel_p++ - 1;
        if (ii < ms->n_rows) {
          for (kk = ms->row_index[ii]; ms->col_id[kk] != jj; kk++);
          mc->val[kk] = xa[2*face_id];
        }
        if (jj < ms->n_rows) {
          for (ll = ms->row_index[jj]; ms->col_id[ll] != ii; ll++);
          mc->val[ll] = xa[2*face_id + 1];
        }
      }
    }

  }
  else { /* if symmetric == true */

    const cs_lnum_t n_faces = matrix->n_faces;
    const cs_lnum_t *restrict face_cel_p = matrix->face_cell;

    for (face_id = 0; face_id < n_faces; face_id++) {
      cs_lnum_t kk, ll;
      ii = *face_cel_p++ - 1;
      jj = *face_cel_p++ - 1;
      if (ii < ms->n_rows) {
        for (kk = ms->row_index[ii]; ms->col_id[kk] != jj; kk++);
        mc->val[kk] = xa[face_id];
      }
      if (jj < ms->n_rows) {
        for (ll = ms->row_index[jj]; ms->col_id[ll] != ii; ll++);
        mc->val[ll] = xa[face_id];
      }

    }

  } /* end of condition on coefficients symmetry */

}

/*----------------------------------------------------------------------------
 * Set CSR extradiagonal matrix coefficients for the case where there are
 * multiple contributions to a given coefficient).
 *
 * The matrix coefficients should have been initialized (i.e. set to 0)
 * some before using this function.
 *
 * parameters:
 *   matrix      <-- Pointer to matrix structure
 *   symmetric   <-- Indicates if extradiagonal values are symmetric
 *   interleaved <-- Indicates if matrix coefficients are interleaved
 *   xa          <-- Extradiagonal values
 *----------------------------------------------------------------------------*/

static void
_set_xa_coeffs_csr_increment(cs_matrix_t      *matrix,
                             bool              symmetric,
                             bool              interleaved,
                             const cs_real_t  *restrict xa)
{
  cs_lnum_t  ii, jj, face_id;
  cs_matrix_coeff_csr_t  *mc = matrix->coeffs;

  const cs_matrix_struct_csr_t  *ms = matrix->structure;

  /* Copy extra-diagonal values */

  assert(matrix->face_cell != NULL);

  if (symmetric == false) {

    const cs_lnum_t n_faces = matrix->n_faces;
    const cs_lnum_t *restrict face_cel_p = matrix->face_cell;

    const cs_real_t  *restrict xa1 = xa;
    const cs_real_t  *restrict xa2 = xa + matrix->n_faces;

    if (interleaved == false) {
      for (face_id = 0; face_id < n_faces; face_id++) {
        cs_lnum_t kk, ll;
        ii = *face_cel_p++ - 1;
        jj = *face_cel_p++ - 1;
        if (ii < ms->n_rows) {
          for (kk = ms->row_index[ii]; ms->col_id[kk] != jj; kk++);
          mc->val[kk] += xa1[face_id];
        }
        if (jj < ms->n_rows) {
          for (ll = ms->row_index[jj]; ms->col_id[ll] != ii; ll++);
          mc->val[ll] += xa2[face_id];
        }
      }
    }
    else { /* interleaved == true */
      for (face_id = 0; face_id < n_faces; face_id++) {
        cs_lnum_t kk, ll;
        ii = *face_cel_p++ - 1;
        jj = *face_cel_p++ - 1;
        if (ii < ms->n_rows) {
          for (kk = ms->row_index[ii]; ms->col_id[kk] != jj; kk++);
          mc->val[kk] += xa[2*face_id];
        }
        if (jj < ms->n_rows) {
          for (ll = ms->row_index[jj]; ms->col_id[ll] != ii; ll++);
          mc->val[ll] += xa[2*face_id + 1];
        }
      }
    }
  }
  else { /* if symmetric == true */

    const cs_lnum_t n_faces = matrix->n_faces;
    const cs_lnum_t *restrict face_cel_p = matrix->face_cell;

    for (face_id = 0; face_id < n_faces; face_id++) {
      cs_lnum_t kk, ll;
      ii = *face_cel_p++ - 1;
      jj = *face_cel_p++ - 1;
      if (ii < ms->n_rows) {
        for (kk = ms->row_index[ii]; ms->col_id[kk] != jj; kk++);
        mc->val[kk] += xa[face_id];
      }
      if (jj < ms->n_rows) {
        for (ll = ms->row_index[jj]; ms->col_id[ll] != ii; ll++);
        mc->val[ll] += xa[face_id];
      }

    }

  } /* end of condition on coefficients symmetry */

}

/*----------------------------------------------------------------------------
 * Set CSR matrix coefficients.
 *
 * parameters:
 *   matrix           <-> Pointer to matrix structure
 *   symmetric        <-- Indicates if extradiagonal values are symmetric
 *   interleaved      <-- Indicates if matrix coefficients are interleaved
 *   copy             <-- Indicates if coefficients should be copied
 *   da               <-- Diagonal values (NULL if all zero)
 *   xa               <-- Extradiagonal values (NULL if all zero)
 *----------------------------------------------------------------------------*/

static void
_set_coeffs_csr(cs_matrix_t      *matrix,
                bool              symmetric,
                bool              interleaved,
                bool              copy,
                const cs_real_t  *restrict da,
                const cs_real_t  *restrict xa)
{
  cs_lnum_t  ii, jj;
  cs_matrix_coeff_csr_t  *mc = matrix->coeffs;

  const cs_matrix_struct_csr_t  *ms = matrix->structure;

  if (mc->val == NULL)
    BFT_MALLOC(mc->val, ms->row_index[ms->n_rows], cs_real_t);

  /* Initialize coefficients to zero if assembly is incremental */

  if (ms->direct_assembly == false) {
    cs_lnum_t val_size = ms->row_index[ms->n_rows];
    for (ii = 0; ii < val_size; ii++)

      mc->val[ii] = 0.0;
  }

  /* Allocate prefetch buffer */

  mc->n_prefetch_rows =  matrix->loop_length;
  if (mc->n_prefetch_rows > 0 && mc->x_prefetch == NULL) {
    size_t prefetch_size = ms->n_cols_max * mc->n_prefetch_rows;
    size_t matrix_size = matrix->n_cells + (2 * matrix->n_faces);
    if (matrix_size > prefetch_size)
      prefetch_size = matrix_size;
    BFT_REALLOC(mc->x_prefetch, prefetch_size, cs_real_t);
  }

  /* Copy diagonal values */

  if (ms->have_diag == true) {

    if (da != NULL) {
      for (ii = 0; ii < ms->n_rows; ii++) {
        cs_lnum_t kk;
        for (kk = ms->row_index[ii]; ms->col_id[kk] != ii; kk++);
        mc->val[kk] = da[ii];
      }
    }
    else {
      for (ii = 0; ii < ms->n_rows; ii++) {
        cs_lnum_t kk;
        for (kk = ms->row_index[ii]; ms->col_id[kk] != ii; kk++);
        mc->val[kk] = 0.0;
      }
    }

  }

  /* Copy extra-diagonal values */

  if (matrix->face_cell != NULL) {

    if (xa != NULL) {

      if (ms->direct_assembly == true)
        _set_xa_coeffs_csr_direct(matrix, symmetric, interleaved, xa);
      else
        _set_xa_coeffs_csr_increment(matrix, symmetric, interleaved, xa);

    }
    else { /* if (xa == NULL) */

      for (ii = 0; ii < ms->n_rows; ii++) {
        const cs_lnum_t  *restrict col_id = ms->col_id + ms->row_index[ii];
        cs_real_t  *m_row = mc->val + ms->row_index[ii];
        cs_lnum_t  n_cols = ms->row_index[ii+1] - ms->row_index[ii];

        for (jj = 0; jj < n_cols; jj++) {
          if (col_id[jj] != ii)
            m_row[jj] = 0.0;
        }

      }

    }

  } /* (matrix->face_cell != NULL) */

}

/*----------------------------------------------------------------------------
 * Release shared CSR matrix coefficients.
 *
 * parameters:
 *   matrix <-- Pointer to matrix structure
 *----------------------------------------------------------------------------*/

static void
_release_coeffs_csr(cs_matrix_t  *matrix)
{
  return;
}

/*----------------------------------------------------------------------------
 * Get diagonal of CSR matrix.
 *
 * parameters:
 *   matrix <-- Pointer to matrix structure
 *   da     --> Diagonal (pre-allocated, size: n_rows)
 *----------------------------------------------------------------------------*/

static void
_get_diagonal_csr(const cs_matrix_t  *matrix,
                  cs_real_t          *restrict da)
{
  cs_lnum_t  ii, jj;
  const cs_matrix_struct_csr_t  *ms = matrix->structure;
  const cs_matrix_coeff_csr_t  *mc = matrix->coeffs;
  cs_lnum_t  n_rows = ms->n_rows;

  if (ms->have_diag == true) {

#   pragma omp parallel for private(jj)
    for (ii = 0; ii < n_rows; ii++) {

      const cs_lnum_t  *restrict col_id = ms->col_id + ms->row_index[ii];
      const cs_real_t  *restrict m_row = mc->val + ms->row_index[ii];
      cs_lnum_t  n_cols = ms->row_index[ii+1] - ms->row_index[ii];

      da[ii] = 0.0;
      for (jj = 0; jj < n_cols; jj++) {
        if (col_id[jj] == ii) {
          da[ii] = m_row[jj];
          break;
        }
      }

    }

  }
  else { /* if (have_diag == false) */
#   pragma omp parallel for
    for (ii = 0; ii < n_rows; ii++)
      da[ii] = 0.0;

  }

}

/*----------------------------------------------------------------------------
 * Local matrix.vector product y = A.x with CSR matrix.
 *
 * parameters:
 *   exclude_diag <-- exclude diagonal if true
 *   matrix       <-- Pointer to matrix structure
 *   x            <-- Multipliying vector values
 *   y            --> Resulting vector
 *----------------------------------------------------------------------------*/

static void
_mat_vec_p_l_csr(bool                exclude_diag,
                 const cs_matrix_t  *matrix,
                 const cs_real_t    *restrict x,
                 cs_real_t          *restrict y)
{
  cs_lnum_t  ii, jj, n_cols;
  cs_real_t  sii;
  cs_lnum_t  *restrict col_id;
  cs_real_t  *restrict m_row;

  const cs_matrix_struct_csr_t  *ms = matrix->structure;
  const cs_matrix_coeff_csr_t  *mc = matrix->coeffs;
  cs_lnum_t  n_rows = ms->n_rows;

  /* Tell IBM compiler not to alias */
# if defined(__xlc__)
# pragma disjoint(*x, *y, *m_row, *col_id)
# endif

  /* Standard case */

  if (!exclude_diag) {

#   pragma omp parallel for private(jj, col_id, m_row, n_cols, sii)
    for (ii = 0; ii < n_rows; ii++) {

      col_id = ms->col_id + ms->row_index[ii];
      m_row = mc->val + ms->row_index[ii];
      n_cols = ms->row_index[ii+1] - ms->row_index[ii];
      sii = 0.0;

      for (jj = 0; jj < n_cols; jj++)
        sii += (m_row[jj]*x[col_id[jj]]);

      y[ii] = sii;

    }

  }

  /* Exclude diagonal */

  else {

#   pragma omp parallel for private(jj, col_id, m_row, n_cols, sii)
    for (ii = 0; ii < n_rows; ii++) {

      col_id = ms->col_id + ms->row_index[ii];
      m_row = mc->val + ms->row_index[ii];
      n_cols = ms->row_index[ii+1] - ms->row_index[ii];
      sii = 0.0;

      for (jj = 0; jj < n_cols; jj++) {
        if (col_id[jj] != ii)
          sii += (m_row[jj]*x[col_id[jj]]);
      }

      y[ii] = sii;

    }
  }

}

#if defined (HAVE_MKL)

static void
_mat_vec_p_l_csr_mkl(bool                exclude_diag,
                     const cs_matrix_t  *matrix,
                     const cs_real_t    *restrict x,
                     cs_real_t          *restrict y)
{
  const cs_matrix_struct_csr_t  *ms = matrix->structure;
  const cs_matrix_coeff_csr_t  *mc = matrix->coeffs;

  int n_rows = ms->n_rows;
  char transa[] = "n";

  if (exclude_diag)
    bft_error(__FILE__, __LINE__, 0,
              _(_no_exclude_diag_error_str), __func__);

  mkl_cspblas_dcsrgemv(transa,
                       &n_rows,
                       mc->val,
                       ms->row_index,
                       ms->col_id,
                       (double *)x,
                       y);
}

#endif /* defined (HAVE_MKL) */

/*----------------------------------------------------------------------------
 * Local matrix.vector product y = A.x with CSR matrix (prefetch).
 *
 * parameters:
 *   exclude_diag <-- exclude diagonal if true
 *   matrix       <-- Pointer to matrix structure
 *   x            <-- Multipliying vector values
 *   y            --> Resulting vector
 *----------------------------------------------------------------------------*/

static void
_mat_vec_p_l_csr_pf(bool                exclude_diag,
                    const cs_matrix_t  *matrix,
                    const cs_real_t    *restrict x,
                    cs_real_t          *restrict y)
{
  cs_lnum_t  start_row, ii, jj, n_cols;
  cs_lnum_t  *restrict col_id;
  cs_real_t  *restrict m_row;

  const cs_matrix_struct_csr_t  *ms = matrix->structure;
  const cs_matrix_coeff_csr_t  *mc = matrix->coeffs;
  cs_lnum_t  n_rows = ms->n_rows;

  if (exclude_diag)
    bft_error(__FILE__, __LINE__, 0,
              _(_no_exclude_diag_error_str), __func__);

  /* Outer loop on prefetch lines */

  for (start_row = 0; start_row < n_rows; start_row += mc->n_prefetch_rows) {

    cs_lnum_t end_row = start_row + mc->n_prefetch_rows;

    cs_real_t  *restrict prefetch_p = mc->x_prefetch;

    /* Tell IBM compiler not to alias */
#   if defined(__xlc__)
#   pragma disjoint(*prefetch_p, *y, *m_row)
#   pragma disjoint(*prefetch_p, *x, *col_id)
#   endif

    if (end_row > n_rows)
      end_row = n_rows;

    /* Prefetch */

    for (ii = start_row; ii < end_row; ii++) {

      col_id = ms->col_id + ms->row_index[ii];
      n_cols = ms->row_index[ii+1] - ms->row_index[ii];

      for (jj = 0; jj < n_cols; jj++)
        *prefetch_p++ = x[col_id[jj]];

    }

    /* Compute */

    prefetch_p = mc->x_prefetch;

    for (ii = start_row; ii < end_row; ii++) {

      cs_real_t  sii = 0.0;

      m_row = mc->val + ms->row_index[ii];
      n_cols = ms->row_index[ii+1] - ms->row_index[ii];

      for (jj = 0; jj < n_cols; jj++)
        sii += *m_row++ * *prefetch_p++;

      y[ii] = sii;

    }

  }

}

/*----------------------------------------------------------------------------
 * Create a symmetric CSR matrix structure from a native matrix stucture.
 *
 * Note that the structure created maps global cell numbers to the given
 * existing face -> cell connectivity array, so it must be destroyed before
 * this array (usually the code's global cell numbering) is freed.
 *
 * parameters:
 *   have_diag   <-- Indicates if the diagonal is nonzero
 *                   (forced to true for symmetric variant)
 *   n_cells     <-- Local number of participating cells
 *   n_cells_ext <-- Local number of cells + ghost cells sharing a face
 *   n_faces     <-- Local number of faces
 *   cell_num    <-- Global cell numbers (1 to n)
 *   face_cell   <-- Face -> cells connectivity (1 to n)
 *
 * returns:
 *   pointer to allocated CSR matrix structure.
 *----------------------------------------------------------------------------*/

static cs_matrix_struct_csr_sym_t *
_create_struct_csr_sym(bool              have_diag,
                       int               n_cells,
                       int               n_cells_ext,
                       int               n_faces,
                       const cs_lnum_t  *face_cell)
{
  int n_cols_max;
  cs_lnum_t ii, jj, face_id;
  const cs_lnum_t *restrict face_cel_p;

  cs_lnum_t  diag_elts = 1;
  cs_lnum_t  *ccount = NULL;

  cs_matrix_struct_csr_sym_t  *ms;

  /* Allocate and map */

  BFT_MALLOC(ms, 1, cs_matrix_struct_csr_sym_t);

  ms->n_rows = n_cells;
  ms->n_cols = n_cells_ext;

  ms->have_diag = have_diag;
  ms->direct_assembly = true;

  BFT_MALLOC(ms->row_index, ms->n_rows + 1, cs_lnum_t);
  ms->row_index = ms->row_index;

  /* Count number of nonzero elements per row */

  BFT_MALLOC(ccount, ms->n_cols, cs_lnum_t);

  if (have_diag == false)
    diag_elts = 0;

  for (ii = 0; ii < ms->n_rows; ii++)  /* count starting with diagonal terms */
    ccount[ii] = diag_elts;

  if (face_cell != NULL) {

    face_cel_p = face_cell;

    for (face_id = 0; face_id < n_faces; face_id++) {
      ii = *face_cel_p++ - 1;
      jj = *face_cel_p++ - 1;
      if (ii < jj)
        ccount[ii] += 1;
      else
        ccount[jj] += 1;
    }

  } /* if (face_cell != NULL) */

  n_cols_max = 0;

  ms->row_index[0] = 0;
  for (ii = 0; ii < ms->n_rows; ii++) {
    ms->row_index[ii+1] = ms->row_index[ii] + ccount[ii];
    if (ccount[ii] > n_cols_max)
      n_cols_max = ccount[ii];
    ccount[ii] = diag_elts; /* pre-count for diagonal terms */
  }

  ms->n_cols_max = n_cols_max;

  /* Build structure */

  BFT_MALLOC(ms->col_id, (ms->row_index[ms->n_rows]), cs_lnum_t);
  ms->col_id = ms->col_id;

  if (have_diag == true) {
    for (ii = 0; ii < ms->n_rows; ii++) {    /* diagonal terms */
      ms->col_id[ms->row_index[ii]] = ii;
    }
  }

  if (face_cell != NULL) {                   /* non-diagonal terms */

    face_cel_p = face_cell;

    for (face_id = 0; face_id < n_faces; face_id++) {
      ii = *face_cel_p++ - 1;
      jj = *face_cel_p++ - 1;
      if (ii < jj && ii < ms->n_rows) {
        ms->col_id[ms->row_index[ii] + ccount[ii]] = jj;
        ccount[ii] += 1;
      }
      else if (ii > jj && jj < ms->n_rows) {
        ms->col_id[ms->row_index[jj] + ccount[jj]] = ii;
        ccount[jj] += 1;
      }
    }

  }

  BFT_FREE(ccount);

  /* Compact elements if necessary */

  if (ms->direct_assembly == false) {

    cs_lnum_t *tmp_row_index = NULL;
    cs_lnum_t  kk = 0;

    BFT_MALLOC(tmp_row_index, ms->n_rows+1, cs_lnum_t);
    memcpy(tmp_row_index, ms->row_index, (ms->n_rows+1)*sizeof(cs_lnum_t));

    kk = 0;

    for (ii = 0; ii < ms->n_rows; ii++) {
      cs_lnum_t *col_id = ms->col_id + ms->row_index[ii];
      cs_lnum_t n_cols = ms->row_index[ii+1] - ms->row_index[ii];
      cs_lnum_t col_id_prev = -1;
      ms->row_index[ii] = kk;
      for (jj = 0; jj < n_cols; jj++) {
        if (col_id_prev != col_id[jj]) {
          ms->col_id[kk++] = col_id[jj];
          col_id_prev = col_id[jj];
        }
      }
    }
    ms->row_index[ms->n_rows] = kk;

    assert(ms->row_index[ms->n_rows] < tmp_row_index[ms->n_rows]);

    BFT_FREE(tmp_row_index);
    BFT_REALLOC(ms->col_id, (ms->row_index[ms->n_rows]), cs_lnum_t);

  }

  return ms;
}

/*----------------------------------------------------------------------------
 * Destroy symmetric CSR matrix structure.
 *
 * parameters:
 *   matrix  <->  Pointer to CSR matrix structure pointer
 *----------------------------------------------------------------------------*/

static void
_destroy_struct_csr_sym(cs_matrix_struct_csr_sym_t  **matrix)
{
  if (matrix != NULL && *matrix !=NULL) {

    cs_matrix_struct_csr_sym_t  *ms = *matrix;

    if (ms->row_index != NULL)
      BFT_FREE(ms->row_index);

    if (ms->col_id != NULL)
      BFT_FREE(ms->col_id);

    BFT_FREE(ms);

    *matrix = ms;

  }
}

/*----------------------------------------------------------------------------
 * Create symmetric CSR matrix coefficients.
 *
 * returns:
 *   pointer to allocated CSR coefficients structure.
 *----------------------------------------------------------------------------*/

static cs_matrix_coeff_csr_sym_t *
_create_coeff_csr_sym(void)
{
  cs_matrix_coeff_csr_sym_t  *mc;

  /* Allocate */

  BFT_MALLOC(mc, 1, cs_matrix_coeff_csr_sym_t);

  /* Initialize */

  mc->val = NULL;

  return mc;
}

/*----------------------------------------------------------------------------
 * Destroy symmetric CSR matrix coefficients.
 *
 * parameters:
 *   coeff  <->  Pointer to CSR matrix coefficients pointer
 *----------------------------------------------------------------------------*/

static void
_destroy_coeff_csr_sym(cs_matrix_coeff_csr_sym_t  **coeff)
{
  if (coeff != NULL && *coeff !=NULL) {

    cs_matrix_coeff_csr_sym_t  *mc = *coeff;

    if (mc->val != NULL)
      BFT_FREE(mc->val);

    BFT_FREE(*coeff);

  }
}

/*----------------------------------------------------------------------------
 * Set symmetric CSR extradiagonal matrix coefficients for the case where
 * direct assignment is possible (i.e. when there are no multiple
 * contributions to a given coefficient).
 *
 * parameters:
 *   matrix    <-- Pointer to matrix structure
 *   xa        <-- Extradiagonal values
 *----------------------------------------------------------------------------*/

static void
_set_xa_coeffs_csr_sym_direct(cs_matrix_t      *matrix,
                              const cs_real_t  *restrict xa)
{
  cs_lnum_t  ii, jj, kk, face_id;
  cs_matrix_coeff_csr_sym_t  *mc = matrix->coeffs;

  const cs_matrix_struct_csr_sym_t  *ms = matrix->structure;
  const cs_lnum_t n_faces = matrix->n_faces;
  const cs_lnum_t *restrict face_cel = matrix->face_cell;

  /* Copy extra-diagonal values */

  assert(matrix->face_cell != NULL);

# pragma omp parallel for private(ii, jj, kk)
  for (face_id = 0; face_id < n_faces; face_id++) {
    ii = face_cel[face_id*2] - 1;
    jj = face_cel[face_id*2 + 1] - 1;
    if (ii < jj && ii < ms->n_rows) {
      for (kk = ms->row_index[ii]; ms->col_id[kk] != jj; kk++);
      mc->val[kk] = xa[face_id];
    }
    else if (ii > jj && jj < ms->n_rows) {
      for (kk = ms->row_index[jj]; ms->col_id[kk] != ii; kk++);
      mc->val[kk] = xa[face_id];
    }
  }
}

/*----------------------------------------------------------------------------
 * Set symmetric CSR extradiagonal matrix coefficients for the case where
 * there are multiple contributions to a given coefficient).
 *
 * The matrix coefficients should have been initialized (i.e. set to 0)
 * some before using this function.
 *
 * parameters:
 *   matrix    <-- Pointer to matrix structure
 *   symmetric <-- Indicates if extradiagonal values are symmetric
 *   xa        <-- Extradiagonal values
 *----------------------------------------------------------------------------*/

static void
_set_xa_coeffs_csr_sym_increment(cs_matrix_t      *matrix,
                                 const cs_real_t  *restrict xa)
{
  cs_lnum_t  ii, jj, face_id;
  cs_matrix_coeff_csr_sym_t  *mc = matrix->coeffs;

  const cs_matrix_struct_csr_sym_t  *ms = matrix->structure;
  const cs_lnum_t n_faces = matrix->n_faces;
  const cs_lnum_t *restrict face_cel_p = matrix->face_cell;

  /* Copy extra-diagonal values */

  assert(matrix->face_cell != NULL);

  for (face_id = 0; face_id < n_faces; face_id++) {
    cs_lnum_t kk;
    ii = *face_cel_p++ - 1;
    jj = *face_cel_p++ - 1;
    if (ii < jj && ii < ms->n_rows) {
      for (kk = ms->row_index[ii]; ms->col_id[kk] != jj; kk++);
      mc->val[kk] += xa[face_id];
    }
    else if (ii > jj && jj < ms->n_rows) {
      for (kk = ms->row_index[jj]; ms->col_id[kk] != ii; kk++);
      mc->val[kk] += xa[face_id];
    }
  }
}

/*----------------------------------------------------------------------------
 * Set symmetric CSR matrix coefficients.
 *
 * parameters:
 *   matrix           <-> Pointer to matrix structure
 *   symmetric        <-- Indicates if extradiagonal values are symmetric (true)
 *   interleaved      <-- Indicates if matrix coefficients are interleaved
 *   copy             <-- Indicates if coefficients should be copied
 *   da               <-- Diagonal values (NULL if all zero)
 *   xa               <-- Extradiagonal values (NULL if all zero)
 *----------------------------------------------------------------------------*/

static void
_set_coeffs_csr_sym(cs_matrix_t      *matrix,
                    bool              symmetric,
                    bool              interleaved,
                    bool              copy,
                    const cs_real_t  *restrict da,
                    const cs_real_t  *restrict xa)
{
  cs_lnum_t  ii, jj;
  cs_matrix_coeff_csr_sym_t  *mc = matrix->coeffs;

  const cs_matrix_struct_csr_sym_t  *ms = matrix->structure;

  if (mc->val == NULL)
    BFT_MALLOC(mc->val, ms->row_index[ms->n_rows], cs_real_t);

  /* Initialize coefficients to zero if assembly is incremental */

  if (ms->direct_assembly == false) {
    cs_lnum_t val_size = ms->row_index[ms->n_rows];
#   pragma omp parallel for
    for (ii = 0; ii < val_size; ii++)
      mc->val[ii] = 0.0;
  }

  /* Copy diagonal values */

  if (ms->have_diag == true) {

    const cs_lnum_t *_diag_index = ms->row_index;

    if (da != NULL) {
#     pragma omp parallel for
      for (ii = 0; ii < ms->n_rows; ii++)
        mc->val[_diag_index[ii]] = da[ii];
    }
    else {
#     pragma omp parallel for
      for (ii = 0; ii < ms->n_rows; ii++)
        mc->val[_diag_index[ii]] = 0.0;
    }

  }

  /* Copy extra-diagonal values */

  if (matrix->face_cell != NULL) {

    if (xa != NULL) {

      if (symmetric == false)
        bft_error(__FILE__, __LINE__, 0,
                  _("Assigning non-symmetric matrix coefficients to a matrix\n"
                    "in a symmetric CSR format."));

      if (ms->direct_assembly == true)
        _set_xa_coeffs_csr_sym_direct(matrix, xa);
      else
        _set_xa_coeffs_csr_sym_increment(matrix, xa);

    }
    else { /* if (xa == NULL) */

      const cs_lnum_t  *restrict col_id;
      cs_real_t  *m_row;
      cs_lnum_t  n_cols;

#     pragma omp parallel for private(jj, col_id, m_row, n_cols)
      for (ii = 0; ii < ms->n_rows; ii++) {
        col_id = ms->col_id + ms->row_index[ii];
        m_row = mc->val + ms->row_index[ii];
        n_cols = ms->row_index[ii+1] - ms->row_index[ii];

        for (jj = 0; jj < n_cols; jj++) {
          if (col_id[jj] != ii)
            m_row[jj] = 0.0;
        }

      }

    }

  } /* (matrix->face_cell != NULL) */

}

/*----------------------------------------------------------------------------
 * Release shared symmetric CSR matrix coefficients.
 *
 * parameters:
 *   matrix <-- Pointer to matrix structure
 *----------------------------------------------------------------------------*/

static void
_release_coeffs_csr_sym(cs_matrix_t  *matrix)
{
  return;
}

/*----------------------------------------------------------------------------
 * Get diagonal of symmetric CSR matrix.
 *
 * parameters:
 *   matrix <-- Pointer to matrix structure
 *   da     --> Diagonal (pre-allocated, size: n_rows)
 *----------------------------------------------------------------------------*/

static void
_get_diagonal_csr_sym(const cs_matrix_t  *matrix,
                      cs_real_t          *restrict da)
{
  cs_lnum_t  ii;
  const cs_matrix_struct_csr_sym_t  *ms = matrix->structure;
  const cs_matrix_coeff_csr_sym_t  *mc = matrix->coeffs;
  cs_lnum_t  n_rows = ms->n_rows;

  if (ms->have_diag == true) {

    /* As structure is symmetric, diagonal values appear first,
       so diag_index == row_index */

    const cs_lnum_t *diag_index = ms->row_index;

    for (ii = 0; ii < n_rows; ii++)
      da[ii] = mc->val[diag_index[ii]];

  }
  else { /* if (have_diag == false) */

    for (ii = 0; ii < n_rows; da[ii++] = 0.0);

  }

}

/*----------------------------------------------------------------------------
 * Local matrix.vector product y = A.x with symmetric CSR matrix.
 *
 * parameters:
 *   exclude_diag <-- exclude diagonal if true
 *   matrix       <-- Pointer to matrix structure
 *   x            <-- Multipliying vector values
 *   y            --> Resulting vector
 *----------------------------------------------------------------------------*/

static void
_mat_vec_p_l_csr_sym(bool                 exclude_diag,
                     const cs_matrix_t   *matrix,
                     const cs_real_t     *restrict x,
                     cs_real_t           *restrict y)
{
  cs_lnum_t  ii, jj, n_cols;
  cs_lnum_t  *restrict col_id;
  cs_real_t  *restrict m_row;

  const cs_matrix_struct_csr_sym_t  *ms = matrix->structure;
  const cs_matrix_coeff_csr_sym_t  *mc = matrix->coeffs;
  cs_lnum_t  n_rows = ms->n_rows;

  cs_lnum_t jj_start = 0;
  cs_lnum_t sym_jj_start = 0;

  /* Tell IBM compiler not to alias */
# if defined(__xlc__)
# pragma disjoint(*x, *y, *m_row, *col_id)
# endif

  /* By construction, the matrix has either a full or an empty
     diagonal structure, so testing this on the first row is enough */

  if (ms->col_id[ms->row_index[0]] == 0) {
    sym_jj_start = 1;
    if (exclude_diag)
      jj_start = 1;
  }

  /* Initialize y */

  for (ii = 0; ii < ms->n_cols; ii++)
    y[ii] = 0.0;

  /* Upper triangular + diagonal part in case of symmetric structure */

  for (ii = 0; ii < n_rows; ii++) {

    cs_real_t  sii = 0.0;

    col_id = ms->col_id + ms->row_index[ii];
    m_row = mc->val + ms->row_index[ii];
    n_cols = ms->row_index[ii+1] - ms->row_index[ii];

    for (jj = jj_start; jj < n_cols; jj++)
      sii += (m_row[jj]*x[col_id[jj]]);

    y[ii] += sii;

    for (jj = sym_jj_start; jj < n_cols; jj++)
      y[col_id[jj]] += (m_row[jj]*x[ii]);
  }

}

#if defined (HAVE_MKL)

static void
_mat_vec_p_l_csr_sym_mkl(bool                exclude_diag,
                         const cs_matrix_t  *matrix,
                         const cs_real_t    *restrict x,
                         cs_real_t          *restrict y)
{
  const cs_matrix_struct_csr_sym_t  *ms = matrix->structure;
  const cs_matrix_coeff_csr_sym_t  *mc = matrix->coeffs;

  int n_rows = ms->n_rows;
  char uplo[] = "u";

  if (exclude_diag)
    bft_error(__FILE__, __LINE__, 0,
              _(_no_exclude_diag_error_str), __func__);

  mkl_cspblas_dcsrsymv(uplo,
                       &n_rows,
                       mc->val,
                       ms->row_index,
                       ms->col_id,
                       (double *)x,
                       y);
}

#endif /* defined (HAVE_MKL) */

/*----------------------------------------------------------------------------
 * Create MSR matrix coefficients.
 *
 * returns:
 *   pointer to allocated MSR coefficients structure.
 *----------------------------------------------------------------------------*/

static cs_matrix_coeff_msr_t *
_create_coeff_msr(void)
{
  cs_matrix_coeff_msr_t  *mc;

  /* Allocate */

  BFT_MALLOC(mc, 1, cs_matrix_coeff_msr_t);

  /* Initialize */

  mc->n_prefetch_rows = 0;
  mc->max_block_size = 0;

  mc->d_val = NULL;

  mc->_d_val = NULL;
  mc->x_val = NULL;

  mc->x_prefetch = NULL;

  return mc;
}

/*----------------------------------------------------------------------------
 * Destroy MSR matrix coefficients.
 *
 * parameters:
 *   coeff  <->  Pointer to MSR matrix coefficients pointer
 *----------------------------------------------------------------------------*/

static void
_destroy_coeff_msr(cs_matrix_coeff_msr_t  **coeff)
{
  if (coeff != NULL && *coeff !=NULL) {

    cs_matrix_coeff_msr_t  *mc = *coeff;

    if (mc->x_prefetch != NULL)
      BFT_FREE(mc->x_prefetch);

    if (mc->x_val != NULL)
      BFT_FREE(mc->x_val);

    if (mc->_d_val != NULL)
      BFT_FREE(mc->_d_val);

    BFT_FREE(*coeff);

  }
}

/*----------------------------------------------------------------------------
 * Set MSR extradiagonal matrix coefficients for the case where direct
 * assignment is possible (i.e. when there are no multiple contributions
 * to a given coefficient).
 *
 * parameters:
 *   matrix      <-- Pointer to matrix structure
 *   symmetric   <-- Indicates if extradiagonal values are symmetric
 *   interleaved <-- Indicates if matrix coefficients are interleaved
 *   xa          <-- Extradiagonal values
 *----------------------------------------------------------------------------*/

static void
_set_xa_coeffs_msr_direct(cs_matrix_t      *matrix,
                          bool              symmetric,
                          bool              interleaved,
                          const cs_real_t  *restrict xa)
{
  cs_lnum_t  ii, jj, face_id;
  cs_matrix_coeff_msr_t  *mc = matrix->coeffs;

  const cs_matrix_struct_csr_t  *ms = matrix->structure;

  /* Copy extra-diagonal values */

  assert(matrix->face_cell != NULL);

  if (symmetric == false) {

    const cs_lnum_t n_faces = matrix->n_faces;
    const cs_lnum_t *restrict face_cel_p = matrix->face_cell;

    const cs_real_t  *restrict xa1 = xa;
    const cs_real_t  *restrict xa2 = xa + matrix->n_faces;

    if (interleaved == false) {
      for (face_id = 0; face_id < n_faces; face_id++) {
        cs_lnum_t kk, ll;
        ii = *face_cel_p++ - 1;
        jj = *face_cel_p++ - 1;
        if (ii < ms->n_rows) {
          for (kk = ms->row_index[ii]; ms->col_id[kk] != jj; kk++);
          mc->x_val[kk] = xa1[face_id];
        }
        if (jj < ms->n_rows) {
          for (ll = ms->row_index[jj]; ms->col_id[ll] != ii; ll++);
          mc->x_val[ll] = xa2[face_id];
        }
      }
    }
    else { /* interleaved == true */
      for (face_id = 0; face_id < n_faces; face_id++) {
        cs_lnum_t kk, ll;
        ii = *face_cel_p++ - 1;
        jj = *face_cel_p++ - 1;
        if (ii < ms->n_rows) {
          for (kk = ms->row_index[ii]; ms->col_id[kk] != jj; kk++);
          mc->x_val[kk] = xa[2*face_id];
        }
        if (jj < ms->n_rows) {
          for (ll = ms->row_index[jj]; ms->col_id[ll] != ii; ll++);
          mc->x_val[ll] = xa[2*face_id + 1];
        }
      }
    }

  }
  else { /* if symmetric == true */

    const cs_lnum_t n_faces = matrix->n_faces;
    const cs_lnum_t *restrict face_cel_p = matrix->face_cell;

    for (face_id = 0; face_id < n_faces; face_id++) {
      cs_lnum_t kk, ll;
      ii = *face_cel_p++ - 1;
      jj = *face_cel_p++ - 1;
      if (ii < ms->n_rows) {
        for (kk = ms->row_index[ii]; ms->col_id[kk] != jj; kk++);
        mc->x_val[kk] = xa[face_id];
      }
      if (jj < ms->n_rows) {
        for (ll = ms->row_index[jj]; ms->col_id[ll] != ii; ll++);
        mc->x_val[ll] = xa[face_id];
      }

    }

  } /* end of condition on coefficients symmetry */

}

/*----------------------------------------------------------------------------
 * Set MSR extradiagonal matrix coefficients for the case where there are
 * multiple contributions to a given coefficient).
 *
 * The matrix coefficients should have been initialized (i.e. set to 0)
 * some before using this function.
 *
 * parameters:
 *   matrix      <-- Pointer to matrix structure
 *   symmetric   <-- Indicates if extradiagonal values are symmetric
 *   interleaved <-- Indicates if matrix coefficients are interleaved
 *   xa          <-- Extradiagonal values
 *----------------------------------------------------------------------------*/

static void
_set_xa_coeffs_msr_increment(cs_matrix_t      *matrix,
                             bool              symmetric,
                             bool              interleaved,
                             const cs_real_t  *restrict xa)
{
  cs_lnum_t  ii, jj, face_id;
  cs_matrix_coeff_msr_t  *mc = matrix->coeffs;

  const cs_matrix_struct_csr_t  *ms = matrix->structure;

  /* Copy extra-diagonal values */

  assert(matrix->face_cell != NULL);

  if (symmetric == false) {

    const cs_lnum_t n_faces = matrix->n_faces;
    const cs_lnum_t *restrict face_cel_p = matrix->face_cell;

    const cs_real_t  *restrict xa1 = xa;
    const cs_real_t  *restrict xa2 = xa + matrix->n_faces;

    if (interleaved == false) {
      for (face_id = 0; face_id < n_faces; face_id++) {
        cs_lnum_t kk, ll;
        ii = *face_cel_p++ - 1;
        jj = *face_cel_p++ - 1;
        if (ii < ms->n_rows) {
          for (kk = ms->row_index[ii]; ms->col_id[kk] != jj; kk++);
          mc->x_val[kk] += xa1[face_id];
        }
        if (jj < ms->n_rows) {
          for (ll = ms->row_index[jj]; ms->col_id[ll] != ii; ll++);
          mc->x_val[ll] += xa2[face_id];
        }
      }
    }
    else { /* interleaved == true */
      for (face_id = 0; face_id < n_faces; face_id++) {
        cs_lnum_t kk, ll;
        ii = *face_cel_p++ - 1;
        jj = *face_cel_p++ - 1;
        if (ii < ms->n_rows) {
          for (kk = ms->row_index[ii]; ms->col_id[kk] != jj; kk++);
          mc->x_val[kk] += xa[2*face_id];
        }
        if (jj < ms->n_rows) {
          for (ll = ms->row_index[jj]; ms->col_id[ll] != ii; ll++);
          mc->x_val[ll] += xa[2*face_id + 1];
        }
      }
    }
  }
  else { /* if symmetric == true */

    const cs_lnum_t n_faces = matrix->n_faces;
    const cs_lnum_t *restrict face_cel_p = matrix->face_cell;

    for (face_id = 0; face_id < n_faces; face_id++) {
      cs_lnum_t kk, ll;
      ii = *face_cel_p++ - 1;
      jj = *face_cel_p++ - 1;
      if (ii < ms->n_rows) {
        for (kk = ms->row_index[ii]; ms->col_id[kk] != jj; kk++);
        mc->x_val[kk] += xa[face_id];
      }
      if (jj < ms->n_rows) {
        for (ll = ms->row_index[jj]; ms->col_id[ll] != ii; ll++);
        mc->x_val[ll] += xa[face_id];
      }

    }

  } /* end of condition on coefficients symmetry */

}

/*----------------------------------------------------------------------------
 * Set MSR matrix coefficients.
 *
 * parameters:
 *   matrix           <-> Pointer to matrix structure
 *   symmetric        <-- Indicates if extradiagonal values are symmetric
 *   interleaved      <-- Indicates if matrix coefficients are interleaved
 *   copy             <-- Indicates if coefficients should be copied
 *   da               <-- Diagonal values (NULL if all zero)
 *   xa               <-- Extradiagonal values (NULL if all zero)
 *----------------------------------------------------------------------------*/

static void
_set_coeffs_msr(cs_matrix_t      *matrix,
                bool              symmetric,
                bool              interleaved,
                bool              copy,
                const cs_real_t  *restrict da,
                const cs_real_t  *restrict xa)
{
  cs_lnum_t  ii, jj;
  cs_matrix_coeff_msr_t  *mc = matrix->coeffs;

  const cs_matrix_struct_csr_t  *ms = matrix->structure;

  /* Allocate prefetch buffer if needed */

  mc->n_prefetch_rows =  matrix->loop_length;
  if (mc->n_prefetch_rows > 0 && mc->x_prefetch == NULL) {
    size_t prefetch_size = ms->n_cols_max * mc->n_prefetch_rows;
    size_t matrix_size = matrix->n_cells + (2 * matrix->n_faces);
    if (matrix_size > prefetch_size)
      prefetch_size = matrix_size;
    BFT_REALLOC(mc->x_prefetch, prefetch_size, cs_real_t);
  }

  /* Map or copy diagonal values */

  if (da != NULL) {

    if (copy) {
      if (mc->_d_val == NULL || mc->max_block_size < matrix->b_size[3]) {
        BFT_REALLOC(mc->_d_val, matrix->b_size[3]*ms->n_rows, cs_real_t);
        mc->max_block_size = matrix->b_size[3];
      }
      memcpy(mc->_d_val, da, matrix->b_size[3]*sizeof(cs_real_t) * ms->n_rows);
      mc->d_val = mc->_d_val;
    }
    else
      mc->d_val = da;

  }
  else
    mc->d_val = NULL;

  /* Extradiagonal values */

  if (mc->x_val == NULL)
    BFT_MALLOC(mc->x_val, ms->row_index[ms->n_rows], cs_real_t);

  /* Initialize coefficients to zero if assembly is incremental */

  if (ms->direct_assembly == false) {
    cs_lnum_t val_size = ms->row_index[ms->n_rows];
    for (ii = 0; ii < val_size; ii++)

      mc->x_val[ii] = 0.0;
  }

  /* Copy extra-diagonal values */

  if (matrix->face_cell != NULL) {

    if (xa != NULL) {

      if (ms->direct_assembly == true)
        _set_xa_coeffs_msr_direct(matrix, symmetric, interleaved, xa);
      else
        _set_xa_coeffs_msr_increment(matrix, symmetric, interleaved, xa);

    }
    else { /* if (xa == NULL) */

      for (ii = 0; ii < ms->n_rows; ii++) {
        const cs_lnum_t  *restrict col_id = ms->col_id + ms->row_index[ii];
        cs_real_t  *m_row = mc->x_val + ms->row_index[ii];
        cs_lnum_t  n_cols = ms->row_index[ii+1] - ms->row_index[ii];

        for (jj = 0; jj < n_cols; jj++) {
          if (col_id[jj] != ii)
            m_row[jj] = 0.0;
        }

      }

    }

  } /* (matrix->face_cell != NULL) */

}

/*----------------------------------------------------------------------------
 * Release shared MSR matrix coefficients.
 *
 * parameters:
 *   matrix <-- Pointer to matrix structure
 *----------------------------------------------------------------------------*/

static void
_release_coeffs_msr(cs_matrix_t  *matrix)
{
  cs_matrix_coeff_msr_t  *mc = matrix->coeffs;
  if (mc !=NULL) {
    /* Unmap shared values */
    mc->d_val = NULL;
  }
}

/*----------------------------------------------------------------------------
 * Local matrix.vector product y = A.x with MSR matrix.
 *
 * parameters:
 *   exclude_diag <-- exclude diagonal if true
 *   matrix       <-- Pointer to matrix structure
 *   x            <-- Multipliying vector values
 *   y            --> Resulting vector
 *----------------------------------------------------------------------------*/

static void
_mat_vec_p_l_msr(bool                exclude_diag,
                 const cs_matrix_t  *matrix,
                 const cs_real_t    *restrict x,
                 cs_real_t          *restrict y)
{
  cs_lnum_t  ii, jj, n_cols;
  cs_real_t  sii;
  cs_lnum_t  *restrict col_id;
  cs_real_t  *restrict m_row;

  const cs_matrix_struct_csr_t  *ms = matrix->structure;
  const cs_matrix_coeff_msr_t  *mc = matrix->coeffs;
  cs_lnum_t  n_rows = ms->n_rows;

  /* Tell IBM compiler not to alias */
# if defined(__xlc__)
# pragma disjoint(*x, *y, *m_row, *col_id)
# endif

  /* Standard case */

  if (!exclude_diag && mc->d_val != NULL) {

#   pragma omp parallel for private(jj, col_id, m_row, n_cols, sii)
    for (ii = 0; ii < n_rows; ii++) {

      col_id = ms->col_id + ms->row_index[ii];
      m_row = mc->x_val + ms->row_index[ii];
      n_cols = ms->row_index[ii+1] - ms->row_index[ii];
      sii = 0.0;

      for (jj = 0; jj < n_cols; jj++)
        sii += (m_row[jj]*x[col_id[jj]]);

      y[ii] = sii + mc->d_val[ii]*x[ii];

    }

  }

  /* Exclude diagonal */

  else {

#   pragma omp parallel for private(jj, col_id, m_row, n_cols, sii)
    for (ii = 0; ii < n_rows; ii++) {

      col_id = ms->col_id + ms->row_index[ii];
      m_row = mc->x_val + ms->row_index[ii];
      n_cols = ms->row_index[ii+1] - ms->row_index[ii];
      sii = 0.0;

      for (jj = 0; jj < n_cols; jj++) {
        if (col_id[jj] != ii)
          sii += (m_row[jj]*x[col_id[jj]]);
      }

      y[ii] = sii;

    }
  }

}

/*----------------------------------------------------------------------------
 * Local matrix.vector product y = A.x with MSR matrix, blocked version.
 *
 * parameters:
 *   exclude_diag <-- exclude diagonal if true
 *   matrix       <-- Pointer to matrix structure
 *   x            <-- Multipliying vector values
 *   y            --> Resulting vector
 *----------------------------------------------------------------------------*/

static void
_b_mat_vec_p_l_msr(bool                exclude_diag,
                   const cs_matrix_t  *matrix,
                   const cs_real_t    *restrict x,
                   cs_real_t          *restrict y)
{
  cs_lnum_t  ii, jj, kk, n_cols;
  cs_lnum_t  *restrict col_id;
  cs_real_t  *restrict m_row;

  const cs_matrix_struct_csr_t  *ms = matrix->structure;
  const cs_matrix_coeff_msr_t  *mc = matrix->coeffs;
  const cs_lnum_t  n_rows = ms->n_rows;
  const int *b_size = matrix->b_size;

  /* Tell IBM compiler not to alias */
# if defined(__xlc__)
# pragma disjoint(*x, *y, *m_row, *col_id)
# endif

  /* Standard case */

  if (!exclude_diag && mc->d_val != NULL) {

#   pragma omp parallel for private(jj, kk, col_id, m_row, n_cols)
    for (ii = 0; ii < n_rows; ii++) {

      col_id = ms->col_id + ms->row_index[ii];
      m_row = mc->x_val + ms->row_index[ii];
      n_cols = ms->row_index[ii+1] - ms->row_index[ii];

      /* Tell IBM compiler not to alias */
#     if defined(__xlc__)
#     pragma disjoint(*x, *y, *m_row, *col_id)
#     endif

      _dense_b_ax(ii, b_size, mc->d_val, x, y);

      for (jj = 0; jj < n_cols; jj++) {
        for (kk = 0; kk < b_size[0]; kk++) {
          y[ii*b_size[1] + kk]
            += (m_row[jj]*x[col_id[jj]*b_size[1] + kk]);
        }
      }

    }

  }

  /* Exclude diagonal */

  else {

#   pragma omp parallel for private(jj, kk, col_id, m_row, n_cols)
    for (ii = 0; ii < n_rows; ii++) {

      col_id = ms->col_id + ms->row_index[ii];
      m_row = mc->x_val + ms->row_index[ii];
      n_cols = ms->row_index[ii+1] - ms->row_index[ii];

      for (kk = 0; kk < b_size[0]; kk++)
        y[ii*b_size[1] + kk] = 0.;

      for (jj = 0; jj < n_cols; jj++) {
        for (kk = 0; kk < b_size[0]; kk++) {
          y[ii*b_size[1] + kk]
            += (m_row[jj]*x[col_id[jj]*b_size[1] + kk]);
        }
      }

    }
  }

}

#if defined (HAVE_MKL)

static void
_mat_vec_p_l_msr_mkl(bool                exclude_diag,
                     const cs_matrix_t  *matrix,
                     const cs_real_t    *restrict x,
                     cs_real_t          *restrict y)
{
  const cs_matrix_struct_csr_t  *ms = matrix->structure;
  const cs_matrix_coeff_msr_t  *mc = matrix->coeffs;

  int n_rows = ms->n_rows;
  char transa[] = "n";

  mkl_cspblas_dcsrgemv(transa,
                       &n_rows,
                       mc->x_val,
                       ms->row_index,
                       ms->col_id,
                       (double *)x,
                       y);

  /* Add diagonal contribution
     TODO: analyse why use of mkl_ddiamv() provides correct results
     in debug (non-optimized) build and in test phase, but leads
     to floating-point exception on some case in optimized build.
     Use of mkl_ddiamv() could provide slightly better performance */

  if (!exclude_diag && mc->d_val != NULL) {
#if 0
    char matdescra[7] = "D NC  ";
    int ndiag = 1;
    int idiag[1] = {0};
    double alpha = 1.0, beta = 1.0;
    mkl_ddiamv(transa,
               &n_rows,
               &n_rows,
               &alpha,
               matdescra,
               (double *)mc->d_val,
               &n_rows,
               idiag,
               &ndiag,
               (double *)x,
               &beta,
               y);
#else
    cs_lnum_t ii;
#   pragma omp parallel for
    for (ii = 0; ii < n_rows; ii++)
      y[ii] += mc->d_val[ii]*x[ii];
#endif
  }
}

#endif /* defined (HAVE_MKL) */

/*----------------------------------------------------------------------------
 * Local matrix.vector product y = A.x with MSR matrix (prefetch).
 *
 * parameters:
 *   exclude_diag <-- exclude diagonal if true
 *   matrix       <-- Pointer to matrix structure
 *   x            <-- Multipliying vector values
 *   y            --> Resulting vector
 *----------------------------------------------------------------------------*/

static void
_mat_vec_p_l_msr_pf(bool                exclude_diag,
                    const cs_matrix_t  *matrix,
                    const cs_real_t    *restrict x,
                    cs_real_t          *restrict y)
{
  cs_lnum_t  start_row, ii, jj, n_cols;
  cs_lnum_t  *restrict col_id;
  cs_real_t  *restrict m_row;
  cs_real_t  *restrict prefetch_p;

  const cs_matrix_struct_csr_t  *ms = matrix->structure;
  const cs_matrix_coeff_msr_t  *mc = matrix->coeffs;
  cs_lnum_t  n_rows = ms->n_rows;

  /* Tell IBM compiler not to alias */
# if defined(__xlc__)
# pragma disjoint(*prefetch_p, *y, *m_row, *col_id)
# endif

  /* Standard case */

  if (!exclude_diag && mc->d_val != NULL) {

    /* Outer loop on prefetch lines */

    for (start_row = 0; start_row < n_rows; start_row += mc->n_prefetch_rows) {

      cs_lnum_t end_row = start_row + mc->n_prefetch_rows;

      prefetch_p = mc->x_prefetch;

      if (end_row > n_rows)
        end_row = n_rows;

      /* Prefetch */

      for (ii = start_row; ii < end_row; ii++) {

        col_id = ms->col_id + ms->row_index[ii];
        n_cols = ms->row_index[ii+1] - ms->row_index[ii];

        for (jj = 0; jj < n_cols; jj++)
          *prefetch_p++ = x[col_id[jj]];

        *prefetch_p++ = x[ii];

      }

      /* Compute */

      prefetch_p = mc->x_prefetch;

      for (ii = start_row; ii < end_row; ii++) {

        cs_real_t  sii = 0.0;

        m_row = mc->x_val + ms->row_index[ii];
        n_cols = ms->row_index[ii+1] - ms->row_index[ii];

        for (jj = 0; jj < n_cols; jj++)
          sii += *m_row++ * *prefetch_p++;

        y[ii] = sii + (mc->d_val[ii] * *prefetch_p++);

      }

    }

  }

  /* Exclude diagonal */

  else {

    /* Outer loop on prefetch lines */

    for (start_row = 0; start_row < n_rows; start_row += mc->n_prefetch_rows) {

      cs_lnum_t end_row = start_row + mc->n_prefetch_rows;

      prefetch_p = mc->x_prefetch;

      if (end_row > n_rows)
        end_row = n_rows;

      /* Prefetch */

      for (ii = start_row; ii < end_row; ii++) {

        col_id = ms->col_id + ms->row_index[ii];
        n_cols = ms->row_index[ii+1] - ms->row_index[ii];

        for (jj = 0; jj < n_cols; jj++)
          *prefetch_p++ = x[col_id[jj]];

      }

      /* Compute */

      prefetch_p = mc->x_prefetch;

      for (ii = start_row; ii < end_row; ii++) {

        cs_real_t  sii = 0.0;

        m_row = mc->x_val + ms->row_index[ii];
        n_cols = ms->row_index[ii+1] - ms->row_index[ii];

        for (jj = 0; jj < n_cols; jj++)
          sii += *m_row++ * *prefetch_p++;

        y[ii] = sii;

      }
    }

  }
}

/*----------------------------------------------------------------------------
 * Create symmetric MSR matrix coefficients.
 *
 * returns:
 *   pointer to allocated MSR coefficients structure.
 *----------------------------------------------------------------------------*/

static cs_matrix_coeff_msr_sym_t *
_create_coeff_msr_sym(void)
{
  cs_matrix_coeff_msr_sym_t  *mc;

  /* Allocate */

  BFT_MALLOC(mc, 1, cs_matrix_coeff_msr_sym_t);

  /* Initialize */

  mc->max_block_size = 0;

  mc->d_val = NULL;

  mc->_d_val = NULL;
  mc->x_val = NULL;

  return mc;
}

/*----------------------------------------------------------------------------
 * Destroy symmetric MSR matrix coefficients.
 *
 * parameters:
 *   coeff  <->  Pointer to MSR matrix coefficients pointer
 *----------------------------------------------------------------------------*/

static void
_destroy_coeff_msr_sym(cs_matrix_coeff_msr_sym_t  **coeff)
{
  if (coeff != NULL && *coeff !=NULL) {

    cs_matrix_coeff_msr_sym_t  *mc = *coeff;

    if (mc->x_val != NULL)
      BFT_FREE(mc->x_val);

    if (mc->_d_val != NULL)
      BFT_FREE(mc->_d_val);

    BFT_FREE(*coeff);

  }
}

/*----------------------------------------------------------------------------
 * Set symmetric MSR extradiagonal matrix coefficients for the case where
 * direct assignment is possible (i.e. when there are no multiple
 * contributions to a given coefficient).
 *
 * parameters:
 *   matrix    <-- Pointer to matrix structure
 *   xa        <-- Extradiagonal values
 *----------------------------------------------------------------------------*/

static void
_set_xa_coeffs_msr_sym_direct(cs_matrix_t      *matrix,
                              const cs_real_t  *restrict xa)
{
  cs_lnum_t  ii, jj, face_id;
  cs_matrix_coeff_msr_sym_t  *mc = matrix->coeffs;

  const cs_matrix_struct_csr_sym_t  *ms = matrix->structure;
  const cs_lnum_t n_faces = matrix->n_faces;
  const cs_lnum_t *restrict face_cel_p = matrix->face_cell;

  /* Copy extra-diagonal values */

  assert(matrix->face_cell != NULL);

  for (face_id = 0; face_id < n_faces; face_id++) {
    cs_lnum_t kk;
    ii = *face_cel_p++ - 1;
    jj = *face_cel_p++ - 1;
    if (ii < jj && ii < ms->n_rows) {
      for (kk = ms->row_index[ii]; ms->col_id[kk] != jj; kk++);
      mc->x_val[kk] = xa[face_id];
    }
    else if (ii > jj && jj < ms->n_rows) {
      for (kk = ms->row_index[jj]; ms->col_id[kk] != ii; kk++);
      mc->x_val[kk] = xa[face_id];
    }
  }
}

/*----------------------------------------------------------------------------
 * Set symmetric MSR extradiagonal matrix coefficients for the case where
 * there are multiple contributions to a given coefficient).
 *
 * The matrix coefficients should have been initialized (i.e. set to 0)
 * some before using this function.
 *
 * parameters:
 *   matrix    <-- Pointer to matrix structure
 *   symmetric <-- Indicates if extradiagonal values are symmetric
 *   xa        <-- Extradiagonal values
 *----------------------------------------------------------------------------*/

static void
_set_xa_coeffs_msr_sym_increment(cs_matrix_t      *matrix,
                                 const cs_real_t  *restrict xa)
{
  cs_lnum_t  ii, jj, face_id;
  cs_matrix_coeff_msr_sym_t  *mc = matrix->coeffs;

  const cs_matrix_struct_csr_sym_t  *ms = matrix->structure;
  const cs_lnum_t n_faces = matrix->n_faces;
  const cs_lnum_t *restrict face_cel_p = matrix->face_cell;

  /* Copy extra-diagonal values */

  assert(matrix->face_cell != NULL);

  for (face_id = 0; face_id < n_faces; face_id++) {
    cs_lnum_t kk;
    ii = *face_cel_p++ - 1;
    jj = *face_cel_p++ - 1;
    if (ii < jj && ii < ms->n_rows) {
      for (kk = ms->row_index[ii]; ms->col_id[kk] != jj; kk++);
      mc->x_val[kk] += xa[face_id];
    }
    else if (ii > jj && jj < ms->n_rows) {
      for (kk = ms->row_index[jj]; ms->col_id[kk] != ii; kk++);
      mc->x_val[kk] += xa[face_id];
    }
  }
}

/*----------------------------------------------------------------------------
 * Set symmetric MSR matrix coefficients.
 *
 * parameters:
 *   matrix           <-> Pointer to matrix structure
 *   symmetric        <-- Indicates if extradiagonal values are symmetric (true)
 *   interleaved      <-- Indicates if matrix coefficients are interleaved
 *   copy             <-- Indicates if coefficients should be copied
 *   da               <-- Diagonal values (NULL if all zero)
 *   xa               <-- Extradiagonal values (NULL if all zero)
 *----------------------------------------------------------------------------*/

static void
_set_coeffs_msr_sym(cs_matrix_t      *matrix,
                    bool              symmetric,
                    bool              interleaved,
                    bool              copy,
                    const cs_real_t  *restrict da,
                    const cs_real_t  *restrict xa)
{
  cs_lnum_t  ii, jj;
  cs_matrix_coeff_msr_sym_t  *mc = matrix->coeffs;

  const cs_matrix_struct_csr_sym_t  *ms = matrix->structure;

  /* Map or copy diagonal values */

  if (da != NULL) {

    if (copy) {
      if (mc->_d_val == NULL || mc->max_block_size < matrix->b_size[3]) {
        BFT_REALLOC(mc->_d_val, matrix->b_size[3]*ms->n_rows, cs_real_t);
        mc->max_block_size = matrix->b_size[3];
      }
      memcpy(mc->_d_val, da, matrix->b_size[3]*sizeof(cs_real_t) * ms->n_rows);
      mc->d_val = mc->_d_val;
    }
    else
      mc->d_val = da;

  }
  else
    mc->d_val = NULL;

  /* Extradiagonal values */

  if (mc->x_val == NULL)
    BFT_MALLOC(mc->x_val, ms->row_index[ms->n_rows], cs_real_t);

  /* Initialize coefficients to zero if assembly is incremental */

  if (ms->direct_assembly == false) {
    cs_lnum_t val_size = ms->row_index[ms->n_rows];
    for (ii = 0; ii < val_size; ii++)
      mc->x_val[ii] = 0.0;
  }

  /* Copy extra-diagonal values */

  if (matrix->face_cell != NULL) {

    if (xa != NULL) {

      if (symmetric == false)
        bft_error(__FILE__, __LINE__, 0,
                  _("Assigning non-symmetric matrix coefficients to a matrix\n"
                    "in a symmetric MSR format."));

      if (ms->direct_assembly == true)
        _set_xa_coeffs_msr_sym_direct(matrix, xa);
      else
        _set_xa_coeffs_msr_sym_increment(matrix, xa);

    }
    else { /* if (xa == NULL) */

      for (ii = 0; ii < ms->n_rows; ii++) {
        const cs_lnum_t  *restrict col_id = ms->col_id + ms->row_index[ii];
        cs_real_t  *m_row = mc->x_val + ms->row_index[ii];
        cs_lnum_t  n_cols = ms->row_index[ii+1] - ms->row_index[ii];

        for (jj = 0; jj < n_cols; jj++) {
          if (col_id[jj] != ii)
            m_row[jj] = 0.0;
        }

      }

    }

  } /* (matrix->face_cell != NULL) */

}

/*----------------------------------------------------------------------------
 * Release shared symmetric MSR matrix coefficients.
 *
 * parameters:
 *   matrix <-- Pointer to matrix structure
 *----------------------------------------------------------------------------*/

static void
_release_coeffs_msr_sym(cs_matrix_t  *matrix)
{
  cs_matrix_coeff_msr_sym_t  *mc = matrix->coeffs;
  if (mc !=NULL) {
    mc->d_val = NULL;
  }
}

/*----------------------------------------------------------------------------
 * Local matrix.vector product y = A.x with symmetric MSR matrix.
 *
 * parameters:
 *   exclude_diag <-- exclude diagonal if true
 *   matrix       <-- Pointer to matrix structure
 *   x            <-- Multipliying vector values
 *   y            --> Resulting vector
 *----------------------------------------------------------------------------*/

static void
_mat_vec_p_l_msr_sym(bool                 exclude_diag,
                     const cs_matrix_t   *matrix,
                     const cs_real_t     *restrict x,
                     cs_real_t           *restrict y)
{
  cs_lnum_t  ii, jj, n_cols;
  cs_lnum_t  *restrict col_id;
  cs_real_t  *restrict m_row;

  const cs_matrix_struct_csr_sym_t  *ms = matrix->structure;
  const cs_matrix_coeff_msr_sym_t  *mc = matrix->coeffs;
  cs_lnum_t  n_rows = ms->n_rows;

  /* Tell IBM compiler not to alias */
# if defined(__xlc__)
# pragma disjoint(*x, *y, *m_row, *col_id)
# endif

  /* Diagonal part of matrix.vector product */

  if (! exclude_diag) {
    _diag_vec_p_l(mc->d_val, x, y, ms->n_rows);
    _zero_range(y, ms->n_rows, ms->n_cols);
  }
  else
    _zero_range(y, 0, ms->n_cols);

  /* Upper triangular + diagonal part in case of symmetric structure */

  for (ii = 0; ii < n_rows; ii++) {

    cs_real_t  sii = 0.0;

    col_id = ms->col_id + ms->row_index[ii];
    m_row = mc->x_val + ms->row_index[ii];
    n_cols = ms->row_index[ii+1] - ms->row_index[ii];

    for (jj = 0; jj < n_cols; jj++)
      sii += (m_row[jj]*x[col_id[jj]]);

    y[ii] += sii;

    for (jj = 0; jj < n_cols; jj++)
      y[col_id[jj]] += (m_row[jj]*x[ii]);
  }

}

#if defined (HAVE_MKL)

static void
_mat_vec_p_l_msr_sym_mkl(bool                exclude_diag,
                         const cs_matrix_t  *matrix,
                         const cs_real_t    *restrict x,
                         cs_real_t          *restrict y)
{
  int ii;
  const cs_matrix_struct_csr_sym_t  *ms = matrix->structure;
  const cs_matrix_coeff_msr_sym_t  *mc = matrix->coeffs;

  int n_rows = ms->n_rows;
  char uplo[] = "u";

  mkl_cspblas_dcsrsymv(uplo,
                       &n_rows,
                       mc->x_val,
                       ms->row_index,
                       ms->col_id,
                       (double *)x,
                       y);

  /* Add diagonal contribution */

  if (!exclude_diag && mc->d_val != NULL) {
    cs_lnum_t ii;
#   pragma omp parallel for
    for (ii = 0; ii < n_rows; ii++)
      y[ii] += mc->d_val[ii]*x[ii];
  }

}

#endif /* defined (HAVE_MKL) */

/*----------------------------------------------------------------------------
 * Synchronize ghost cells prior to matrix.vector product
 *
 * parameters:
 *   rotation_mode <-- Halo update option for rotational periodicity
 *   matrix        <-- Pointer to matrix structure
 *   x             <-> Multipliying vector values (ghost values updated)
 *   y             --> Resulting vector
 *----------------------------------------------------------------------------*/

static void
_pre_vector_multiply_sync(cs_perio_rota_t     rotation_mode,
                          const cs_matrix_t  *matrix,
                          cs_real_t          *restrict x,
                          cs_real_t          *restrict y)
{
  size_t n_cells_ext = matrix->n_cells_ext;

  assert(matrix->halo != NULL);

  /* Non-blocked version */

  if (matrix->b_size[3] == 1) {

    /* Synchronize for parallelism and periodicity first */

    _zero_range(y, matrix->n_cells, n_cells_ext);

    /* Update distant ghost cells */

    if (matrix->halo != NULL) {

      cs_halo_sync_var(matrix->halo, CS_HALO_STANDARD, x);

      /* Synchronize periodic values */

      if (matrix->halo->n_transforms > 0) {
        if (rotation_mode == CS_PERIO_ROTA_IGNORE)
          bft_error(__FILE__, __LINE__, 0, _cs_glob_perio_ignore_error_str);
        cs_perio_sync_var_scal(matrix->halo, CS_HALO_STANDARD, rotation_mode, x);
      }

    }

  }

  /* Blocked version */

  else { /* if (matrix->b_size[3] > 1) */

    const int *b_size = matrix->b_size;

    /* Synchronize for parallelism and periodicity first */

    _b_zero_range(y, matrix->n_cells, n_cells_ext, b_size);

    /* Update distant ghost cells */

    if (matrix->halo != NULL) {

      cs_halo_sync_var_strided(matrix->halo,
                               CS_HALO_STANDARD,
                               x,
                               b_size[1]);

      /* Synchronize periodic values */

      if (matrix->halo->n_transforms > 0 && b_size[0] == 3)
        cs_perio_sync_var_vect(matrix->halo, CS_HALO_STANDARD, x, b_size[1]);

    }

  }
}

/*----------------------------------------------------------------------------
 * Copy array to reference for matrix computation check.
 *
 * parameters:
 *   n_elts      <-- number values to compare
 *   y           <-- array to copare or copy
 *   yr          <-- reference array
 *
 * returns:
 *   maximum difference between values
 *----------------------------------------------------------------------------*/

static double
_matrix_check_compare(cs_lnum_t        n_elts,
                      const cs_real_t  y[],
                      cs_real_t        yr[])
{
  cs_lnum_t  ii;

  double dmax = 0.0;

  for (ii = 0; ii < n_elts; ii++) {
    double d = CS_ABS(y[ii] - yr[ii]);
    if (d > dmax)
      dmax = d;
  }

#if defined(HAVE_MPI)

  if (cs_glob_n_ranks > 1) {
    double dmaxg;
    MPI_Allreduce(&dmax, &dmaxg, 1, MPI_DOUBLE, MPI_MAX, cs_glob_mpi_comm);
    dmax = dmaxg;
  }

#endif

  return dmax;
}

/*----------------------------------------------------------------------------
 * Check local matrix.vector product operations.
 *
 * parameters:
 *   t_measure   <-- minimum time for each measure
 *   n_variants  <-- number of variants in array
 *   n_cells     <-- number of local cells
 *   n_cells_ext <-- number of cells including ghost cells (array size)
 *   n_faces     <-- local number of internal faces
 *   cell_num    <-- global cell numbers (1 to n)
 *   face_cell   <-- face -> cells connectivity (1 to n)
 *   halo        <-- cell halo structure
 *   numbering   <-- vectorization or thread-related numbering info, or NULL
 *   m_variant   <-> array of matrix variants
 *----------------------------------------------------------------------------*/

static void
_matrix_check(int                    n_variants,
              cs_lnum_t              n_cells,
              cs_lnum_t              n_cells_ext,
              cs_lnum_t              n_faces,
              const cs_gnum_t       *cell_num,
              const cs_lnum_t       *face_cell,
              const cs_halo_t       *halo,
              const cs_numbering_t  *numbering,
              cs_matrix_variant_t   *m_variant)
{
  cs_lnum_t  ii;
  int  v_id, b_id, ed_flag;
  int  sym_flag;

  cs_real_t  *da = NULL, *xa = NULL, *x = NULL, *y = NULL;
  cs_real_t  *yr0 = NULL, *yr1 = NULL;
  cs_matrix_structure_t *ms = NULL;
  cs_matrix_t *m = NULL;
  int diag_block_size[4] = {3, 3, 3, 9};

  /* Allocate and initialize  working arrays */

  if (CS_MEM_ALIGN > 0) {
    BFT_MEMALIGN(x, CS_MEM_ALIGN, n_cells_ext*diag_block_size[1], cs_real_t);
    BFT_MEMALIGN(y, CS_MEM_ALIGN, n_cells_ext*diag_block_size[1], cs_real_t);
    BFT_MEMALIGN(yr0, CS_MEM_ALIGN, n_cells_ext*diag_block_size[1], cs_real_t);
    BFT_MEMALIGN(yr1, CS_MEM_ALIGN, n_cells_ext*diag_block_size[1], cs_real_t);
  }
  else {
    BFT_MALLOC(x, n_cells_ext*diag_block_size[1], cs_real_t);
    BFT_MALLOC(y, n_cells_ext*diag_block_size[1], cs_real_t);
    BFT_MALLOC(yr0, n_cells_ext*diag_block_size[1], cs_real_t);
    BFT_MALLOC(yr1, n_cells_ext*diag_block_size[1], cs_real_t);
  }

  BFT_MALLOC(da, n_cells_ext*diag_block_size[3], cs_real_t);
  BFT_MALLOC(xa, n_faces*2, cs_real_t);

  /* Initialize arrays */

# pragma omp parallel for
  for (ii = 0; ii < n_cells_ext*diag_block_size[3]; ii++)
    da[ii] = 1.0 + cos(ii);

# pragma omp parallel for
  for (ii = 0; ii < n_faces; ii++) {
    xa[ii*2] = 0.5*(0.9 + cos(ii));
    xa[ii*2 + 1] = -0.5*(0.9 + cos(ii));
  }

# pragma omp parallel for
  for (ii = 0; ii < n_cells_ext*diag_block_size[1]; ii++)
    x[ii] = sin(ii);

  /* Loop on block sizes */

  for (b_id = 0; b_id < 2; b_id++) {

    const int *_diag_block_size = (b_id == 0) ? NULL : diag_block_size;
    const cs_lnum_t _block_mult = (b_id == 0) ? 1 : diag_block_size[1];

    /* Loop on symmetry and diagonal exclusion flags */

    for (sym_flag = 0; sym_flag < 2; sym_flag++) {

      bool sym_coeffs = (sym_flag == 0) ? false : true;

      for (ed_flag = 0; ed_flag < 2; ed_flag++) {

        /* Loop on variant types */

        for (v_id = 0; v_id < n_variants; v_id++) {

          cs_matrix_variant_t *v = m_variant + v_id;

          cs_matrix_vector_product_t        *vector_multiply = NULL;

          if (sym_flag == 0) {
            if (v->symmetry == 1)
              continue;
          }
          else {
            if (v->symmetry == 0)
              continue;
          }

          ms = cs_matrix_structure_create(v->type,
                                          true,
                                          n_cells,
                                          n_cells_ext,
                                          n_faces,
                                          cell_num,
                                          face_cell,
                                          halo,
                                          numbering);
          m = cs_matrix_create(ms);

          m->loop_length = v->loop_length;

          /* Ignore unhandled cases */

          if (sym_flag + v->symmetry == 1) /* sym_flag xor v->symmetry */
            continue;

          cs_matrix_set_coefficients(m,
                                     sym_coeffs,
                                     _diag_block_size,
                                     da,
                                     xa);

          /* Check other operations */

          vector_multiply = v->vector_multiply[b_id*2 + ed_flag];

          if (vector_multiply != NULL) {
            vector_multiply(ed_flag, m, x, y);
            if (v_id == 0)
              memcpy(yr0, y, n_cells*_block_mult*sizeof(cs_real_t));
            else {
              double dmax = _matrix_check_compare(n_cells*_block_mult, y, yr0);
              bft_printf("%-32s %-32s : %12.5e\n",
                         v->name,
                         _matrix_operation_name[b_id*4 + sym_flag*2 + ed_flag],
                         dmax);
              bft_printf_flush();
            }
          }

          cs_matrix_release_coefficients(m);
          cs_matrix_destroy(&m);
          cs_matrix_structure_destroy(&ms);

        } /* end of loop on variants */

      } /* end of loop on ed_flag */

    } /* end of loop on sym_flag */

  } /* end of loop on block sizes */

  BFT_FREE(yr1);
  BFT_FREE(yr0);

  BFT_FREE(y);
  BFT_FREE(x);

  BFT_FREE(xa);
  BFT_FREE(da);
}

/*----------------------------------------------------------------------------
 * Tune local matrix.vector product operations.
 *
 * parameters:
 *   t_measure   <-- minimum time for each measure
 *   n_variants  <-- number of variants in array
 *   n_cells     <-- number of local cells
 *   n_cells_ext <-- number of cells including ghost cells (array size)
 *   n_faces     <-- local number of internal faces
 *   cell_num    <-- global cell numbers (1 to n)
 *   face_cell   <-- face -> cells connectivity (1 to n)
 *   halo        <-- cell halo structure
 *   numbering   <-- vectorization or thread-related numbering info, or NULL
 *   m_variant   <-> array of matrix variants
 *----------------------------------------------------------------------------*/

static void
_matrix_tune_test(double                 t_measure,
                  int                    n_variants,
                  cs_lnum_t              n_cells,
                  cs_lnum_t              n_cells_ext,
                  cs_lnum_t              n_faces,
                  const cs_gnum_t       *cell_num,
                  const cs_lnum_t       *face_cell,
                  const cs_halo_t       *halo,
                  const cs_numbering_t  *numbering,
                  cs_matrix_variant_t   *m_variant)
{
  cs_lnum_t  ii;
  int  n_runs, run_id, v_id, b_id, ed_flag;
  double  wt0, wt1, wtu;
  int  sym_flag, sym_start, sym_end;
  cs_matrix_type_t  type, type_prev;

  double test_sum = 0.0;
  cs_real_t  *da = NULL, *xa = NULL, *x = NULL, *y = NULL, *z = NULL;
  cs_matrix_structure_t *ms = NULL;
  cs_matrix_t *m = NULL;
  int diag_block_size[4] = {3, 3, 3, 9};

  type_prev = CS_MATRIX_N_TYPES;

  /* Allocate and initialize  working arrays */
  /*-----------------------------------------*/

  if (CS_MEM_ALIGN > 0) {
    BFT_MEMALIGN(x, CS_MEM_ALIGN, n_cells_ext*diag_block_size[1], cs_real_t);
    BFT_MEMALIGN(y, CS_MEM_ALIGN, n_cells_ext*diag_block_size[1], cs_real_t);
    BFT_MEMALIGN(z, CS_MEM_ALIGN, n_cells_ext*diag_block_size[1], cs_real_t);
  }
  else {
    BFT_MALLOC(x, n_cells_ext*diag_block_size[1], cs_real_t);
    BFT_MALLOC(y, n_cells_ext*diag_block_size[1], cs_real_t);
    BFT_MALLOC(z, n_cells_ext*diag_block_size[1], cs_real_t);
  }

  BFT_MALLOC(da, n_cells_ext*diag_block_size[3], cs_real_t);
  BFT_MALLOC(xa, n_faces*2, cs_real_t);

# pragma omp parallel for
  for (ii = 0; ii < n_cells_ext*diag_block_size[3]; ii++)
    da[ii] = 1.0 + ii/n_cells_ext;
# pragma omp parallel for
  for (ii = 0; ii < n_cells_ext*diag_block_size[1]; ii++) {
    x[ii] = ii/n_cells_ext;
    z[ii] = ii/n_cells_ext;
  }

# pragma omp parallel for
  for (ii = 0; ii < n_faces; ii++) {
    xa[ii*2] = 0.5*(1.0 + ii/n_faces);
    xa[ii*2 + 1] = -0.5*(1.0 + ii/n_faces);
  }

  /* Loop on variant types */
  /*-----------------------*/

  for (v_id = 0; v_id < n_variants; v_id++) {

    bool test_assign = false;

    cs_matrix_variant_t *v = m_variant + v_id;

    type = v->type;

    sym_start = (v->symmetry % 2 == 0) ? 0 : 1;
    sym_end = (v->symmetry > 0) ? 2 : 1;

    if (type != type_prev) {

      test_assign = true;

      wt0 = cs_timer_wtime(), wt1 = wt0;
      run_id = 0, n_runs = 8;
      while (run_id < n_runs) {
        while (run_id < n_runs) {
          if (m != NULL)
            cs_matrix_destroy(&m);
          if (ms != NULL)
            cs_matrix_structure_destroy(&ms);
          ms = cs_matrix_structure_create(type,
                                          true,
                                          n_cells,
                                          n_cells_ext,
                                          n_faces,
                                          cell_num,
                                          face_cell,
                                          halo,
                                          numbering);
          m = cs_matrix_create(ms);
          run_id++;
        }
        wt1 = cs_timer_wtime();
        if (wt1 - wt0 < t_measure)
          n_runs *= 2;
      }
      v->matrix_create_cost = (wt1 - wt0) / n_runs;
    }

    m->loop_length = v->loop_length;

    /* Loop on block sizes */

    for (b_id = 0; b_id < 2; b_id++) {

      const int *_diag_block_size = (b_id == 0) ? NULL : diag_block_size;

      /* Loop on symmetry and diagonal exclusion flags */

      for (sym_flag = sym_start; sym_flag < sym_end; sym_flag++) {

        bool sym_coeffs = (sym_flag == 0) ? false : true;
        double t_measure_assign = -1;

        for (ed_flag = 0; ed_flag < 2; ed_flag++) {

          cs_matrix_vector_product_t        *vector_multiply = NULL;

          /* Ignore unhandled cases */

          if (sym_flag + v->symmetry == 1) /* sym_flag xor v->symmetry */
            continue;

          /* Measure overhead of setting coefficients if not already done */

          if (test_assign && ed_flag == 0) {
            t_measure_assign = t_measure;
            n_runs = 8;
          }
          else
            n_runs = 1;

          wt0 = cs_timer_wtime(), wt1 = wt0;
          run_id = 0;
          while (run_id < n_runs) {
            while (run_id < n_runs) {
              cs_matrix_set_coefficients(m,
                                         sym_coeffs,
                                         _diag_block_size,
                                         da,
                                         xa);
              run_id++;
            }
            wt1 = cs_timer_wtime();
            if (wt1 - wt0 < t_measure_assign)
              n_runs *= 2;
          }
          if (n_runs > 1)
            v->matrix_assign_cost[b_id*2 + sym_flag] = (wt1 - wt0) / n_runs;

          /* Measure other operations */

          vector_multiply = v->vector_multiply[b_id*2 + ed_flag];

          if (vector_multiply != NULL) {
            wt0 = cs_timer_wtime(), wt1 = wt0;
            run_id = 0, n_runs = 8;
            while (run_id < n_runs) {
              while (run_id < n_runs) {
                if (run_id % 8)
                  test_sum = 0;
                vector_multiply(ed_flag, m, x, y);
                test_sum += y[n_cells-1];
                run_id++;
              }
              wt1 = cs_timer_wtime();
              if (wt1 - wt0 < t_measure)
                n_runs *= 2;
            }
            wtu = (wt1 - wt0) / n_runs;
            v->matrix_vector_cost[b_id*4 + sym_flag*2 + ed_flag] = wtu;
          }

          cs_matrix_release_coefficients(m);

        } /* end of loop on ed_flag */

      } /* end of loop on sym_flag */

    } /* end of loop on block sizes */

    type_prev = type;

  } /* end of loop on variants */

  if (m != NULL)
    cs_matrix_destroy(&m);
  if (ms != NULL)
    cs_matrix_structure_destroy(&ms);

  BFT_FREE(x);
  BFT_FREE(y);
  BFT_FREE(z);

  BFT_FREE(da);
  BFT_FREE(xa);
}

/*----------------------------------------------------------------------------
 * Print title for statistics on matrix tuning SpMv info.
 *
 * parameters:
 *   struct_flag <-- 0: assignment; 1: structure creation
 *   sym_flag    <-- 0: non-symmetric only; 1; symmetric only
 *   block_flag  <-- 0: no blocks; 1; blocks only
 *----------------------------------------------------------------------------*/

static void
_matrix_tune_create_assign_title(int  struct_flag,
                                 int  sym_flag,
                                 int  block_flag)
{
  size_t i = 0;
  size_t l = 80;
  char title[81] = "";

  /* Print title */

  if (struct_flag == 0) {
    if (sym_flag) {
      strncat(title + i, _("symmetric "), l);
      title[80] = '\0';
      i = strlen(title);
      l -= i;
    }
    if (block_flag && l > 0) {
      strncat(title + i, _("block "), l);
      title[80] = '\0';
      i = strlen(title);
      l -= i;
    }
    strncat(title + i, _("matrix coefficients assign"), l);
  }
  else
    strncat(title + i, _("matrix structure creation/destruction"), l);

  title[80] = '\0';

  l = cs_log_strlen(title);

  cs_log_printf(CS_LOG_PERFORMANCE, "\n%s\n", title);

  for (i = 0; i < l; i++)
    title[i] = '-';
  title[l] = '\0';

  cs_log_printf(CS_LOG_PERFORMANCE, "%s\n", title);

  /* Compute local ratios */

#if defined(HAVE_MPI)

  if (cs_glob_n_ranks > 1) {

    char tmp_s[4][24] =  {"", "", "", ""};

    cs_log_strpadl(tmp_s[0], _("time (s)"), 16, 24);
    cs_log_strpadl(tmp_s[1], _("mean"), 12, 24);
    cs_log_strpadl(tmp_s[2], _("min"), 12, 24);
    cs_log_strpadl(tmp_s[3], _("max"), 12, 24);

    cs_log_printf(CS_LOG_PERFORMANCE,
                  "  %24s %21s %s\n"
                  "  %24s %s %s %s\n",
                  " ", " ", tmp_s[0],
                  " ", tmp_s[1], tmp_s[2], tmp_s[3]);
  }

#endif

  if (cs_glob_n_ranks == 1) {

    char tmp_s[24] =  {""};

    cs_log_strpadl(tmp_s, _("time (s)"), 12, 24);

    cs_log_printf(CS_LOG_PERFORMANCE,
                  "  %24s %s\n",
                  " ", tmp_s);

  }
}

/*----------------------------------------------------------------------------
 * Print statistics on matrix tuning creation or assignment info.
 *
 * parameters:
 *   m_variant   <-- array of matrix variants
 *   variant_id  <-- variant id
 *   struct_flag <-- 0: assignment; 1: structure creation
 *   sym_flag    <-- 0: non-symmetric only; 1; symmetric only
 *   block_flag  <-- 0: no blocks; 1; blocks only
 *----------------------------------------------------------------------------*/

static void
_matrix_tune_create_assign_stats(const cs_matrix_variant_t  *m_variant,
                                 int                         variant_id,
                                 int                         struct_flag,
                                 int                         sym_flag,
                                 int                         block_flag)
{
  char title[32];

  double t_loc = -1;

  const cs_matrix_variant_t  *v = m_variant + variant_id;

  cs_log_strpad(title, v->name, 24, 32);

  if (struct_flag == 0)
    t_loc = v->matrix_assign_cost[block_flag* 2 + sym_flag];
  else
    t_loc = v->matrix_create_cost;

  if (t_loc < 0)
    return;

#if defined(HAVE_MPI)

  if (cs_glob_n_ranks > 1) {
    double t_max, t_min, t_sum = -1;
    MPI_Allreduce(&t_loc, &t_sum, 1, MPI_DOUBLE, MPI_SUM, cs_glob_mpi_comm);
    MPI_Allreduce(&t_loc, &t_min, 1, MPI_DOUBLE, MPI_MIN, cs_glob_mpi_comm);
    MPI_Allreduce(&t_loc, &t_max, 1, MPI_DOUBLE, MPI_MAX, cs_glob_mpi_comm);
    cs_log_printf(CS_LOG_PERFORMANCE,
                  "  %s %12.5e %12.5e %12.5e\n",
                  title, t_sum/cs_glob_n_ranks, t_min, t_max);
  }

#endif

  if (cs_glob_n_ranks == 1)
    cs_log_printf(CS_LOG_PERFORMANCE,
                  "  %s %12.5e\n", title, t_loc);
}

/*----------------------------------------------------------------------------
 * Print title for statistics on matrix tuning SpMv info.
 *
 * parameters:
 *   sym_flag    <-- 0: non-symmetric only; 1; symmetric only
 *   ed_flag     <-- 0: include diagonal; 1: exclude diagonal
 *   block_flag  <-- 0: no blocks; 1; blocks only
 *----------------------------------------------------------------------------*/

static void
_matrix_tune_spmv_title(int  sym_flag,
                        int  ed_flag,
                        int  block_flag)
{
  size_t i = 0;
  size_t l = 80;
  char title[81] = "";

  /* Print title */

  snprintf(title, 80, "%s",
           _(_matrix_operation_name[block_flag*4 + sym_flag*2 + ed_flag]));
  title[80] = '\0';
  l = cs_log_strlen(title);

  cs_log_printf(CS_LOG_PERFORMANCE, "\n%s\n", title);

  for (i = 0; i < l; i++)
    title[i] = '-';
  title[l] = '\0';

  cs_log_printf(CS_LOG_PERFORMANCE, "%s\n", title);

  /* Compute local ratios */

#if defined(HAVE_MPI)

  if (cs_glob_n_ranks > 1) {

    char tmp_s[8][24] =  {"", "", "", "", "", "", "", ""};

    cs_log_strpadl(tmp_s[0], _("time (s)"), 16, 24);
    cs_log_strpadl(tmp_s[1], _("speedup"), 16, 24);
    cs_log_strpadl(tmp_s[2], _("mean"), 12, 24);
    cs_log_strpadl(tmp_s[3], _("min"), 12, 24);
    cs_log_strpadl(tmp_s[4], _("max"), 12, 24);
    cs_log_strpadl(tmp_s[5], _("mean"), 8, 24);
    cs_log_strpadl(tmp_s[6], _("min"), 8, 24);
    cs_log_strpadl(tmp_s[7], _("max"), 8, 24);

    cs_log_printf(CS_LOG_PERFORMANCE,
                  "  %24s %21s %s %9s %s\n"
                  "  %24s %s %s %s %s %s %s\n",
                  " ", " ", tmp_s[0], " ", tmp_s[1],
                  " ", tmp_s[2], tmp_s[3], tmp_s[4],
                  tmp_s[5], tmp_s[6], tmp_s[7]);
  }

#endif

  if (cs_glob_n_ranks == 1) {

    char tmp_s[2][24] =  {"", ""};

    cs_log_strpadl(tmp_s[0], _("time (s)"), 12, 24);
    cs_log_strpadl(tmp_s[1], _("speedup"), 8, 24);

    cs_log_printf(CS_LOG_PERFORMANCE,
                  "  %24s %s %s\n",
                  " ", tmp_s[0], tmp_s[1]);

  }
}

/*----------------------------------------------------------------------------
 * Print statistics on matrix tuning SpMv info.
 *
 * parameters:
 *   m_variant   <-- array of matrix variants
 *   variant_id  <-- variant id
 *   sym_flag    <-- 0: non-symmetric only; 1; symmetric only
 *   ed_flag     <-- 0: include diagonal; 1: exclude diagonal
 *   block_flag  <-- 0: no blocks; 1; blocks only
 *----------------------------------------------------------------------------*/

static void
_matrix_tune_spmv_stats(const cs_matrix_variant_t  *m_variant,
                        int                         variant_id,
                        int                         sym_flag,
                        int                         ed_flag,
                        int                         block_flag)
{
  char title[32];

  int sub_id = block_flag*4 + sym_flag*2 + ed_flag;

  double v_loc[2] = {-1, -1};

  const cs_matrix_variant_t  *r = m_variant;
  const cs_matrix_variant_t  *v = m_variant + variant_id;

  cs_log_strpad(title, v->name, 24, 32);

  /* Get timing info */

  v_loc[0] = v->matrix_vector_cost[sub_id];
  v_loc[1] = r->matrix_vector_cost[sub_id] / v_loc[0];

  if (v_loc[0] < 0)
    return;

  /* Compute local ratios */

#if defined(HAVE_MPI)

  if (cs_glob_n_ranks > 1) {
    double v_max[2], v_min[2], v_sum[2];
    MPI_Allreduce(v_loc, v_sum, 2, MPI_DOUBLE, MPI_SUM, cs_glob_mpi_comm);
    MPI_Allreduce(v_loc, v_min, 2, MPI_DOUBLE, MPI_MIN, cs_glob_mpi_comm);
    MPI_Allreduce(v_loc, v_max, 2, MPI_DOUBLE, MPI_MAX, cs_glob_mpi_comm);
    cs_log_printf(CS_LOG_PERFORMANCE,
                  "  %s %12.5e %12.5e %12.5e %8.4f %8.4f %8.4f\n",
                  title,
                  v_sum[0]/cs_glob_n_ranks, v_min[0], v_max[0],
                  (v_sum[1]/cs_glob_n_ranks), v_min[1], v_max[1]);
  }

#endif

  if (cs_glob_n_ranks == 1)
    cs_log_printf(CS_LOG_PERFORMANCE,
                  "  %s %12.5e %8.4f\n",
                  title,
                  v_loc[0], v_loc[1]);
}

/*----------------------------------------------------------------------------
 * Initialize local variant matrix.
 *
 * parameters:
 *   v  <-> pointer to matrix variant
 *----------------------------------------------------------------------------*/

static void
_variant_init(cs_matrix_variant_t  *v)
{
  int i;

  v->matrix_create_cost = -1.;

  for (i = 0; i < 4; i++) {
    v->vector_multiply[i] = NULL;
    v->matrix_assign_cost[i] = -1.;
  }

  for (i = 0; i < 8; i++)
    v->matrix_vector_cost[i] = -1.;
}

/*----------------------------------------------------------------------------
 * Add variant
 *
 * parameters:
 *   name                 <-- matrix variant name
 *   type                 <-- matrix type
 *   block_flag           <-- 0: non-block only, 1: block only, 2: both
 *   sym_flag             <-- 0: non-symmetric only, 1: symmetric only, 2: both
 *   ed_flag              <-- 0: with diagonal only, 1 exclude only; 2; both
 *   loop_length          <-- loop length option for some algorithms
 *   vector_multiply      <-- function pointer for A.x
 *   b_vector_multiply    <-- function pointer for block A.x
 *   n_variants           <-> number of variants
 *   n_variants_max       <-> current maximum number of variants
 *   m_variant            <-> array of matrix variants
 *----------------------------------------------------------------------------*/

static void
_variant_add(const char                        *name,
             cs_matrix_type_t                   type,
             int                                block_flag,
             int                                sym_flag,
             int                                ed_flag,
             int                                loop_length,
             cs_matrix_vector_product_t        *vector_multiply,
             cs_matrix_vector_product_t        *b_vector_multiply,
             int                               *n_variants,
             int                               *n_variants_max,
             cs_matrix_variant_t              **m_variant)
{
  cs_matrix_variant_t  *v;
  int i = *n_variants;

  if (*n_variants_max == *n_variants) {
    if (*n_variants_max == 0)
      *n_variants_max = 8;
    else
      *n_variants_max *= 2;
    BFT_REALLOC(*m_variant, *n_variants_max, cs_matrix_variant_t);
  }

  v = (*m_variant) + i;

  _variant_init(v);

  strcpy(v->name, name);
  v->type = type;
  v->symmetry = sym_flag;
  v->loop_length = loop_length;

  if (block_flag != 1) {
    if (ed_flag != 1)
      v->vector_multiply[0] = vector_multiply;
    if (ed_flag != 0)
      v->vector_multiply[1] = vector_multiply;
  }

  if (block_flag != 0) {
    if (ed_flag != 1)
      v->vector_multiply[2] = b_vector_multiply;
    if (ed_flag != 0)
      v->vector_multiply[3] = b_vector_multiply;
  }

  *n_variants += 1;
}

/*----------------------------------------------------------------------------
 * Build list of variants for tuning or testing.
 *
 * parameters:
 *   sym_flag             <-- 0: non-symmetric only, 1: symmetric only, 2: both
 *   block_flag           <-- 0: non-block only, 1: block only, 2: both
 *   n_variants           --> number of variants
 *   m_variant            --> array of matrix variants
 *----------------------------------------------------------------------------*/

static void
_build_variant_list(int                    sym_flag,
                    int                    block_flag,
                    int                   *n_variants,
                    cs_matrix_variant_t  **m_variant)
{
  int  n_variants_max = 0;

  *n_variants = 0;
  *m_variant = NULL;

  _variant_add(_("Native, baseline"),
               CS_MATRIX_NATIVE,
               block_flag,
               sym_flag,
               2, /* ed_flag */
               0, /* loop_length */
               _mat_vec_p_l_native,
               _b_mat_vec_p_l_native,
               n_variants,
               &n_variants_max,
               m_variant);

  _variant_add(_("Native, 3x3 blocks"),
               CS_MATRIX_NATIVE,
               block_flag,
               sym_flag,
               2, /* ed_flag */
               0, /* loop_length */
               NULL,
               _3_3_mat_vec_p_l_native,
               n_variants,
               &n_variants_max,
               m_variant);

  _variant_add(_("Native, Bull algorithm"),
               CS_MATRIX_NATIVE,
               block_flag,
               sym_flag,
               2, /* ed_flag */
               508, /* loop_length */
               _mat_vec_p_l_native_bull,
               NULL,
               n_variants,
               &n_variants_max,
               m_variant);

  _variant_add(_("CSR"),
               CS_MATRIX_CSR,
               block_flag,
               sym_flag,
               2, /* ed_flag */
               0, /* loop_length */
               _mat_vec_p_l_csr,
               NULL,
               n_variants,
               &n_variants_max,
               m_variant);

  _variant_add(_("CSR, with prefetch"),
               CS_MATRIX_CSR,
               block_flag,
               sym_flag,
               0, /* ed_flag */
               508, /* loop_length */
               _mat_vec_p_l_csr_pf,
               NULL,
               n_variants,
               &n_variants_max,
               m_variant);

#if defined(HAVE_MKL)

  _variant_add(_("CSR, with MKL"),
               CS_MATRIX_CSR,
               block_flag,
               sym_flag,
               0, /* ed_flag */
               0, /* loop_length */
               _mat_vec_p_l_csr_mkl,
               NULL,
               n_variants,
               &n_variants_max,
               m_variant);

#endif /* defined(HAVE_MKL) */

  if (sym_flag == 1) {

    _variant_add(_("CSR_SYM"),
                 CS_MATRIX_CSR_SYM,
                 block_flag,
                 sym_flag,
                 2, /* ed_flag */
                 0, /* loop_length */
                 _mat_vec_p_l_csr_sym,
                 NULL,
                 n_variants,
                 &n_variants_max,
                 m_variant);

#if defined(HAVE_MKL)

    _variant_add(_("CSR_SYM, with MKL"),
                 CS_MATRIX_CSR_SYM,
                 block_flag,
                 sym_flag,
                 0, /* ed_flag */
                 0, /* loop_length */
                 _mat_vec_p_l_csr_sym_mkl,
                 NULL,
                 n_variants,
                 &n_variants_max,
                 m_variant);

#endif /* defined(HAVE_MKL) */

  }

  _variant_add(_("MSR"),
               CS_MATRIX_MSR,
               block_flag,
               sym_flag,
               2, /* ed_flag */
               0, /* loop_length */
               _mat_vec_p_l_msr,
               _b_mat_vec_p_l_msr,
               n_variants,
               &n_variants_max,
               m_variant);

  _variant_add(_("MSR, with prefetch"),
               CS_MATRIX_MSR,
               block_flag,
               sym_flag,
               2, /* ed_flag */
               508, /* loop_length */
               _mat_vec_p_l_msr_pf,
               NULL,
               n_variants,
               &n_variants_max,
               m_variant);

#if defined(HAVE_MKL)

  _variant_add(_("MSR, with MKL"),
               CS_MATRIX_MSR,
               block_flag,
               sym_flag,
               2, /* ed_flag */
               0, /* loop_length */
               _mat_vec_p_l_msr_mkl,
               NULL,
               n_variants,
               &n_variants_max,
               m_variant);

#endif /* defined(HAVE_MKL) */

  if (sym_flag == 1) {

    _variant_add(_("MSR_SYM"),
                 CS_MATRIX_MSR_SYM,
                 block_flag,
                 sym_flag,
                 2, /* ed_flag */
                 0, /* loop_length */
                 _mat_vec_p_l_msr_sym,
                 NULL,
                 n_variants,
                 &n_variants_max,
                 m_variant);

#if defined(HAVE_MKL)

    _variant_add(_("MSR_SYM, with MKL"),
                 CS_MATRIX_MSR_SYM,
                 block_flag,
                 sym_flag,
                 2, /* ed_flag */
                 0, /* loop_length */
                 _mat_vec_p_l_msr_sym_mkl,
                 NULL,
                 n_variants,
                 &n_variants_max,
                 m_variant);

#endif /* defined(HAVE_MKL) */

  }

  n_variants_max = *n_variants;
  BFT_REALLOC(*m_variant, *n_variants, cs_matrix_variant_t);
}

/*============================================================================
 *  Public function definitions for Fortran API
 *============================================================================*/

void CS_PROCF(promav, PROMAV)
(
 const cs_int_t   *ncelet,    /* <-- Number of cells, halo included */
 const cs_int_t   *ncel,      /* <-- Number of local cells */
 const cs_int_t   *nfac,      /* <-- Number of faces */
 const cs_int_t   *isym,      /* <-- Symmetry indicator:
                                     1: symmetric; 2: not symmetric */
 const cs_int_t   *ibsize,    /* <-- Block size of element ii, ii */
 const cs_int_t   *iinvpe,    /* <-- Indicator to cancel increments
                                     in rotational periodicty (2) or
                                     to exchange them as scalars (1) */
 const cs_int_t   *ifacel,    /* <-- Face -> cell connectivity  */
 const cs_real_t  *dam,       /* <-- Matrix diagonal */
 const cs_real_t  *xam,       /* <-- Matrix extra-diagonal terms */
 cs_real_t        *vx,        /* <-- A*vx */
 cs_real_t        *vy         /* <-> vy = A*vx */
)
{
  int diag_block_size[4] = {1, 1, 1, 1};
  bool symmetric = (*isym == 1) ? true : false;
  cs_perio_rota_t rotation_mode = CS_PERIO_ROTA_COPY;

  assert(*ncelet >= *ncel);
  assert(*nfac > 0);
  assert(ifacel != NULL);

  if (*iinvpe == 2)
    rotation_mode = CS_PERIO_ROTA_RESET;
  else if (*iinvpe == 3)
    rotation_mode = CS_PERIO_ROTA_IGNORE;

  if (*ibsize > 1 || symmetric) {
    /* TODO: update diag_block_size[] values for the general case */
    diag_block_size[0] = *ibsize;
    diag_block_size[1] = *ibsize;
    diag_block_size[2] = *ibsize;
    diag_block_size[3] = (*ibsize)*(*ibsize);
    cs_matrix_set_coefficients(cs_glob_matrix_default,
                               symmetric,
                               diag_block_size,
                               dam,
                               xam);
  }
  else
    cs_matrix_set_coefficients_ni(cs_glob_matrix_default, false, dam, xam);

  cs_matrix_vector_multiply(rotation_mode,
                            cs_glob_matrix_default,
                            vx,
                            vy);
}

/*============================================================================
 * Public function definitions
 *============================================================================*/

/*----------------------------------------------------------------------------
 * Initialize sparse matrix API.
 *----------------------------------------------------------------------------*/

void
cs_matrix_initialize(void)
{
  cs_mesh_t  *mesh = cs_glob_mesh;

  assert(mesh != NULL);

  cs_glob_matrix_default_struct
    = cs_matrix_structure_create(CS_MATRIX_NATIVE,
                                 true,
                                 mesh->n_cells,
                                 mesh->n_cells_with_ghosts,
                                 mesh->n_i_faces,
                                 mesh->global_cell_num,
                                 mesh->i_face_cells,
                                 mesh->halo,
                                 mesh->i_face_numbering);

  cs_glob_matrix_default
    = cs_matrix_create(cs_glob_matrix_default_struct);
}

/*----------------------------------------------------------------------------
 * Finalize sparse matrix API.
 *----------------------------------------------------------------------------*/

void
cs_matrix_finalize(void)
{
  cs_matrix_destroy(&cs_glob_matrix_default);
  cs_matrix_structure_destroy(&cs_glob_matrix_default_struct);
}

/*----------------------------------------------------------------------------
 * Create a matrix Structure.
 *
 * Note that the structure created maps to the given existing
 * cell global number, face -> cell connectivity arrays, and cell halo
 * structure, so it must be destroyed before they are freed
 * (usually along with the code's main face -> cell structure).
 *
 * Note that the resulting matrix structure will contain either a full or
 * an empty main diagonal, and that the extra-diagonal structure is always
 * symmetric (though the coefficients my not be, and we may choose a
 * matrix format that does not exploit ths symmetry). If the face_cell
 * connectivity argument is NULL, the matrix will be purely diagonal.
 *
 * parameters:
 *   type        <-- Type of matrix considered
 *   have_diag   <-- Indicates if the diagonal structure contains nonzeroes
 *   n_cells     <-- Local number of cells
 *   n_cells_ext <-- Local number of cells + ghost cells sharing a face
 *   n_faces     <-- Local number of internal faces
 *   cell_num    <-- Global cell numbers (1 to n)
 *   face_cell   <-- Face -> cells connectivity (1 to n)
 *   halo        <-- Halo structure associated with cells, or NULL
 *   numbering   <-- vectorization or thread-related numbering info, or NULL
 *
 * returns:
 *   pointer to created matrix structure;
 *----------------------------------------------------------------------------*/

cs_matrix_structure_t *
cs_matrix_structure_create(cs_matrix_type_t       type,
                           bool                   have_diag,
                           cs_lnum_t              n_cells,
                           cs_lnum_t              n_cells_ext,
                           cs_lnum_t              n_faces,
                           const cs_gnum_t       *cell_num,
                           const cs_lnum_t       *face_cell,
                           const cs_halo_t       *halo,
                           const cs_numbering_t  *numbering)
{
  cs_matrix_structure_t *ms;

  BFT_MALLOC(ms, 1, cs_matrix_structure_t);

  ms->type = type;

  ms->n_cells = n_cells;
  ms->n_cells_ext = n_cells_ext;
  ms->n_faces = n_faces;

  /* Define Structure */

  switch(ms->type) {
  case CS_MATRIX_NATIVE:
    ms->structure = _create_struct_native(n_cells,
                                          n_cells_ext,
                                          n_faces,
                                          face_cell);
    break;
  case CS_MATRIX_CSR:
    ms->structure = _create_struct_csr(have_diag,
                                       n_cells,
                                       n_cells_ext,
                                       n_faces,
                                       face_cell);
    break;
  case CS_MATRIX_CSR_SYM:
    ms->structure = _create_struct_csr_sym(have_diag,
                                           n_cells,
                                           n_cells_ext,
                                           n_faces,
                                           face_cell);
    break;
  case CS_MATRIX_MSR:
    ms->structure = _create_struct_csr(false,
                                       n_cells,
                                       n_cells_ext,
                                       n_faces,
                                       face_cell);
    break;
  case CS_MATRIX_MSR_SYM:
    ms->structure = _create_struct_csr_sym(false,
                                           n_cells,
                                           n_cells_ext,
                                           n_faces,
                                           face_cell);
    break;
  default:
    bft_error(__FILE__, __LINE__, 0,
              _("Handling of matrixes in %s format\n"
                "is not operational yet."),
              _(cs_matrix_type_name[type]));
    break;
  }

  /* Set pointers to structures shared from mesh here */

  ms->face_cell = face_cell;
  ms->cell_num = cell_num;
  ms->halo = halo;
  ms->numbering = numbering;

  return ms;
}

/*----------------------------------------------------------------------------
 * Destroy a matrix structure.
 *
 * parameters:
 *   ms <-> Pointer to matrix structure pointer
 *----------------------------------------------------------------------------*/

void
cs_matrix_structure_destroy(cs_matrix_structure_t  **ms)
{
  if (ms != NULL && *ms != NULL) {

    cs_matrix_structure_t *_ms = *ms;

    switch(_ms->type) {
    case CS_MATRIX_NATIVE:
      {
        cs_matrix_struct_native_t *structure = _ms->structure;
        _destroy_struct_native(&structure);
      }
      break;
    case CS_MATRIX_CSR:
      {
        cs_matrix_struct_csr_t *structure = _ms->structure;
        _destroy_struct_csr(&structure);
      }
      break;
    case CS_MATRIX_CSR_SYM:
      {
        cs_matrix_struct_csr_sym_t *structure = _ms->structure;
        _destroy_struct_csr_sym(&structure);
      }
      break;
    case CS_MATRIX_MSR:
      {
        cs_matrix_struct_csr_t *structure = _ms->structure;
        _destroy_struct_csr(&structure);
      }
      break;
    case CS_MATRIX_MSR_SYM:
      {
        cs_matrix_struct_csr_sym_t *structure = _ms->structure;
        _destroy_struct_csr_sym(&structure);
      }
      break;
    default:
      assert(0);
      break;
    }
    _ms->structure = NULL;

    /* Now free main structure */

    BFT_FREE(*ms);
  }
}

/*----------------------------------------------------------------------------
 * Create a matrix container using a given structure.
 *
 * Note that the matrix container maps to the assigned structure,
 * so it must be destroyed before that structure.
 *
 * parameters:
 *   ms <-- Associated matrix structure
 *
 * returns:
 *   pointer to created matrix structure;
 *----------------------------------------------------------------------------*/

cs_matrix_t *
cs_matrix_create(const cs_matrix_structure_t  *ms)
{
  int i;
  cs_matrix_t *m;

  BFT_MALLOC(m, 1, cs_matrix_t);

  m->type = ms->type;

  /* Map shared structure */

  m->n_cells = ms->n_cells;
  m->n_cells_ext = ms->n_cells_ext;
  m->n_faces = ms->n_faces;

  for (i = 0; i < 4; i++)
    m->b_size[i] = 1;

  m->structure = ms->structure;

  m->face_cell = ms->face_cell;
  m->cell_num = ms->cell_num;
  m->halo = ms->halo;
  m->numbering = ms->numbering;

  /* Set default loop length for some algorithms; a size slightly less
     than a multiple of cache size is preferred */

  m->loop_length = 508;

  /* Define coefficients */

  switch(m->type) {
  case CS_MATRIX_NATIVE:
    m->coeffs = _create_coeff_native();
    break;
  case CS_MATRIX_CSR:
    m->coeffs = _create_coeff_csr();
    break;
  case CS_MATRIX_CSR_SYM:
    m->coeffs = _create_coeff_csr_sym();
    break;
  case CS_MATRIX_MSR:
    m->coeffs = _create_coeff_msr();
    break;
  case CS_MATRIX_MSR_SYM:
    m->coeffs = _create_coeff_msr_sym();
    break;
  default:
    bft_error(__FILE__, __LINE__, 0,
              _("Handling of matrixes in %s format\n"
                "is not operational yet."),
              _(cs_matrix_type_name[m->type]));
    break;
  }

  /* Set function pointers here */

  m->set_coefficients = NULL;
  m->vector_multiply[0] = NULL;
  m->vector_multiply[2] = NULL;

  switch(m->type) {

  case CS_MATRIX_NATIVE:

    m->set_coefficients = _set_coeffs_native;
    m->release_coefficients = _release_coeffs_native;
    m->get_diagonal = _get_diagonal_separate;
    m->vector_multiply[0] = _mat_vec_p_l_native;
    m->vector_multiply[2] = _b_mat_vec_p_l_native;

    /* Optimized variants here */

#if defined(IA64_OPTIM)
    m->vector_multiply[0] = _mat_vec_p_l_native_bull;
#endif

    if (m->numbering != NULL) {
#if defined(HAVE_OPENMP)
      if (m->numbering->type == CS_NUMBERING_THREADS) {
        m->vector_multiply[0] = _mat_vec_p_l_native_omp;
        m->vector_multiply[2] = _b_mat_vec_p_l_native_omp;
      }
#endif
#if defined(SX) && defined(_SX) /* For vector machines */
      if (m->numbering->type == CS_NUMBERING_VECTORIZE) {
        m->vector_multiply[0] = _mat_vec_p_l_native_vector;
      }
#endif
    }

    break;

  case CS_MATRIX_CSR:
    m->set_coefficients = _set_coeffs_csr;
    m->release_coefficients = _release_coeffs_csr;
    m->get_diagonal = _get_diagonal_csr;
    if (m->loop_length > 0 && cs_glob_n_threads == 1) {
      m->vector_multiply[0] = _mat_vec_p_l_csr_pf;
    }
    else {
      m->vector_multiply[0] = _mat_vec_p_l_csr;
    }
    break;

  case CS_MATRIX_CSR_SYM:
    m->set_coefficients = _set_coeffs_csr_sym;
    m->release_coefficients = _release_coeffs_csr_sym;
    m->get_diagonal = _get_diagonal_csr_sym;
    m->vector_multiply[0] = _mat_vec_p_l_csr_sym;
    break;

  case CS_MATRIX_MSR:
    m->set_coefficients = _set_coeffs_msr;
    m->release_coefficients = _release_coeffs_msr;
    m->get_diagonal = _get_diagonal_separate;
    if (m->loop_length > 0 && cs_glob_n_threads == 1) {
      m->vector_multiply[0] = _mat_vec_p_l_msr_pf;
    }
    else {
      m->vector_multiply[0] = _mat_vec_p_l_msr;
    }
    break;

  case CS_MATRIX_MSR_SYM:
    m->set_coefficients = _set_coeffs_msr_sym;
    m->release_coefficients = _release_coeffs_msr_sym;
    m->get_diagonal = _get_diagonal_separate;
    m->vector_multiply[0] = _mat_vec_p_l_msr_sym;
    break;

  default:
    assert(0);

  }

  m->vector_multiply[1] = m->vector_multiply[0];
  m->vector_multiply[3] = m->vector_multiply[2];

  return m;
}

/*----------------------------------------------------------------------------
 * Create a matrix container using a given structure and tuning info.
 *
 * If the matrix variant is incompatible with the structure, it is ignored.
 *
 * parameters:
 *   ms <-- Associated matrix structure
 *   mv <-- Associated matrix variant
 *
 * returns:
 *   pointer to created matrix structure;
 *----------------------------------------------------------------------------*/

cs_matrix_t *
cs_matrix_create_tuned(const cs_matrix_structure_t  *ms,
                       const cs_matrix_variant_t    *mv)
{
  cs_matrix_t *m = cs_matrix_create(ms);

  if (mv != NULL) {
    if (mv->type == ms->type) {
      int i;
      m->loop_length = mv->loop_length;
      for (i = 0; i < 4; i++) {
        if (mv->vector_multiply[i] != NULL)
          m->vector_multiply[i] = mv->vector_multiply[i];
      }
    }
  }

  return m;
}

/*----------------------------------------------------------------------------
 * Destroy a matrix structure.
 *
 * parameters:
 *   matrix <-> Pointer to matrix structure pointer
 *----------------------------------------------------------------------------*/

void
cs_matrix_destroy(cs_matrix_t **matrix)
{
  if (matrix != NULL && *matrix != NULL) {

    cs_matrix_t *m = *matrix;

    switch(m->type) {
    case CS_MATRIX_NATIVE:
      {
        cs_matrix_coeff_native_t *coeffs = m->coeffs;
        _destroy_coeff_native(&coeffs);
      }
      break;
    case CS_MATRIX_CSR:
      {
        cs_matrix_coeff_csr_t *coeffs = m->coeffs;
        _destroy_coeff_csr(&coeffs);
        m->coeffs = NULL;
      }
      break;
    case CS_MATRIX_CSR_SYM:
      {
        cs_matrix_coeff_csr_sym_t *coeffs = m->coeffs;
        _destroy_coeff_csr_sym(&coeffs);
        m->coeffs = NULL;
      }
      break;
    case CS_MATRIX_MSR:
      {
        cs_matrix_coeff_msr_t *coeffs = m->coeffs;
        _destroy_coeff_msr(&coeffs);
        m->coeffs = NULL;
      }
      break;
    case CS_MATRIX_MSR_SYM:
      {
        cs_matrix_coeff_msr_sym_t *coeffs = m->coeffs;
        _destroy_coeff_msr_sym(&coeffs);
        m->coeffs = NULL;
      }
      break;
    default:
      assert(0);
      break;
    }

    m->coeffs = NULL;

    /* Now free main structure */

    BFT_FREE(*matrix);
  }
}

/*----------------------------------------------------------------------------
 * Return number of columns in matrix.
 *
 * parameters:
 *   matrix <-- Pointer to matrix structure
 *----------------------------------------------------------------------------*/

cs_lnum_t
cs_matrix_get_n_columns(const cs_matrix_t  *matrix)
{
  if (matrix == NULL)
    bft_error(__FILE__, __LINE__, 0,
              _("The matrix is not defined."));
  return matrix->n_cells_ext;
}

/*----------------------------------------------------------------------------
 * Return number of rows in matrix.
 *
 * parameters:
 *   matrix <-- Pointer to matrix structure
 *----------------------------------------------------------------------------*/

cs_lnum_t
cs_matrix_get_n_rows(const cs_matrix_t  *matrix)
{
  if (matrix == NULL)
    bft_error(__FILE__, __LINE__, 0,
              _("The matrix is not defined."));
  return matrix->n_cells;
}

/*----------------------------------------------------------------------------
 * Return matrix diagonal block sizes.
 *
 * Block sizes are defined by a array of 4 values:
 *   0: useful block size, 1: vector block extents,
 *   2: matrix line extents,  3: matrix line*column extents
 *
 * parameters:
 *   matrix <-- Pointer to matrix structure
 *
 * returns:
 *   pointer to block sizes
 *----------------------------------------------------------------------------*/

const int *
cs_matrix_get_diag_block_size(const cs_matrix_t  *matrix)
{
  if (matrix == NULL)
    bft_error(__FILE__, __LINE__, 0,
              _("The matrix is not defined."));
  if (   matrix->type == CS_MATRIX_CSR
      || matrix->type == CS_MATRIX_CSR_SYM
      || matrix->type == CS_MATRIX_MSR_SYM)
    bft_error(__FILE__, __LINE__, 0,
              _("Not supported with %."),
              _(cs_matrix_type_name[matrix->type]));

  return matrix->b_size;
}

/*----------------------------------------------------------------------------
 * Set matrix coefficients, sharing arrays with the caller when possible.
 *
 * With shared arrays, the matrix becomes unusable if the arrays passed as
 * arguments are not be modified (its coefficients should be unset first
 * to mark this).
 *
 * Depending on current options and initialization, values will be copied
 * or simply mapped.
 *
 * Block sizes are defined by an optional array of 4 values:
 *   0: useful block size, 1: vector block extents,
 *   2: matrix line extents,  3: matrix line*column extents
 *
 * parameters:
 *   matrix           <-> Pointer to matrix structure
 *   symmetric        <-- Indicates if matrix coefficients are symmetric
 *   diag_block_size  <-- Block sizes for diagonal, or NULL
 *   da               <-- Diagonal values (NULL if zero)
 *   xa               <-- Extradiagonal values (NULL if zero)
 *----------------------------------------------------------------------------*/

void
cs_matrix_set_coefficients(cs_matrix_t      *matrix,
                           bool              symmetric,
                           const int        *diag_block_size,
                           const cs_real_t  *da,
                           const cs_real_t  *xa)
{
  int i;

  if (matrix == NULL)
    bft_error(__FILE__, __LINE__, 0,
              _("The matrix is not defined."));

  if (diag_block_size == NULL) {
    for (i = 0; i < 4; i++)
      matrix->b_size[i] = 1;
  }
  else {
    for (i = 0; i < 4; i++)
      matrix->b_size[i] = diag_block_size[i];
  }

  if (matrix->set_coefficients != NULL)
    matrix->set_coefficients(matrix, symmetric, true, false, da, xa);
}

/*----------------------------------------------------------------------------
 * Set matrix coefficients in the non-interleaved case.
 *
 * In the symmetric case, there is no difference with the interleaved case.
 *
 * Depending on current options and initialization, values will be copied
 * or simply mapped (non-symmetric values will be copied).
 *
 * parameters:
 *   matrix    <-> Pointer to matrix structure
 *   symmetric <-- Indicates if matrix coefficients are symmetric
 *   da        <-- Diagonal values (NULL if zero)
 *   xa        <-- Extradiagonal values (NULL if zero)
 *----------------------------------------------------------------------------*/

void
cs_matrix_set_coefficients_ni(cs_matrix_t      *matrix,
                              bool              symmetric,
                              const cs_real_t  *da,
                              const cs_real_t  *xa)
{
  int i;

  if (matrix == NULL)
    bft_error(__FILE__, __LINE__, 0,
              _("The matrix is not defined."));

  for (i = 0; i < 4; i++)
    matrix->b_size[i] = 1;

  if (matrix->set_coefficients != NULL)
    matrix->set_coefficients(matrix, symmetric, false, false, da, xa);
}

/*----------------------------------------------------------------------------
 * Set matrix coefficients, copying values to private arrays.
 *
 * With private arrays, the matrix becomes independant from the
 * arrays passed as arguments.
 *
 * Block sizes are defined by an optional array of 4 values:
 *   0: useful block size, 1: vector block extents,
 *   2: matrix line extents,  3: matrix line*column extents
 *
 * parameters:
 *   matrix           <-> Pointer to matrix structure
 *   symmetric        <-- Indicates if matrix coefficients are symmetric
 *   diag_block_size  <-- Block sizes for diagonal, or NULL
 *   da               <-- Diagonal values (NULL if zero)
 *   xa               <-- Extradiagonal values (NULL if zero)
 *----------------------------------------------------------------------------*/

void
cs_matrix_copy_coefficients(cs_matrix_t      *matrix,
                            bool              symmetric,
                            const int        *diag_block_size,
                            const cs_real_t  *da,
                            const cs_real_t  *xa)
{
  int i;

  if (matrix == NULL)
    bft_error(__FILE__, __LINE__, 0,
              _("The matrix is not defined."));

  if (diag_block_size == NULL) {
    for (i = 0; i < 4; i++)
      matrix->b_size[i] = 1;
  }
  else {
    for (i = 0; i < 4; i++)
      matrix->b_size[i] = diag_block_size[i];
  }

  if (matrix->set_coefficients != NULL)
    matrix->set_coefficients(matrix, symmetric, true, true, da, xa);
}

/*----------------------------------------------------------------------------
 * Release shared matrix coefficients.
 *
 * Pointers to mapped coefficients are set to NULL, while
 * coefficient copies owned by the matrix are not modified.
 *
 * This simply ensures the matrix does not maintain pointers
 * to nonexistant data.
 *
 * parameters:
 *   matrix <-> Pointer to matrix structure
 *----------------------------------------------------------------------------*/

void
cs_matrix_release_coefficients(cs_matrix_t  *matrix)
{
  /* Check API state */

  if (matrix == NULL)
    bft_error(__FILE__, __LINE__, 0,
              _("The matrix is not defined."));

  if (matrix->release_coefficients != NULL)
    matrix->release_coefficients(matrix);
}

/*----------------------------------------------------------------------------
 * Get matrix diagonal values.
 *
 * parameters:
 *   matrix <-- Pointer to matrix structure
 *   da     --> Diagonal (pre-allocated, size: n_cells)
 *----------------------------------------------------------------------------*/

void
cs_matrix_get_diagonal(const cs_matrix_t  *matrix,
                       cs_real_t          *restrict da)
{
  /* Check API state */

  if (matrix == NULL)
    bft_error(__FILE__, __LINE__, 0,
              _("The matrix is not defined."));

  if (matrix->get_diagonal != NULL)
    matrix->get_diagonal(matrix, da);
}

/*----------------------------------------------------------------------------
 * Matrix.vector product y = A.x
 *
 * This function includes a halo update of x prior to multiplication by A.
 *
 * parameters:
 *   rotation_mode <-- Halo update option for rotational periodicity
 *   matrix        <-- Pointer to matrix structure
 *   x             <-> Multipliying vector values (ghost values updated)
 *   y             --> Resulting vector
 *----------------------------------------------------------------------------*/

void
cs_matrix_vector_multiply(cs_perio_rota_t     rotation_mode,
                          const cs_matrix_t  *matrix,
                          cs_real_t          *restrict x,
                          cs_real_t          *restrict y)
{
  assert(matrix != NULL);

  if (matrix->halo != NULL)
    _pre_vector_multiply_sync(rotation_mode,
                              matrix,
                              x,
                              y);

  /* Non-blocked version */

  if (matrix->b_size[3] == 1) {
    if (matrix->vector_multiply[0] != NULL)
      matrix->vector_multiply[0](false, matrix, x, y);
    else
      bft_error(__FILE__, __LINE__, 0,
                _("Matrix is missing a vector multiply function."));
  }

  /* Blocked version */

  else {
    if (matrix->vector_multiply[2] != NULL)
      matrix->vector_multiply[2](false, matrix, x, y);
    else
      bft_error(__FILE__, __LINE__, 0,
                _("Block matrix is missing a vector multiply function."));
  }
}

/*----------------------------------------------------------------------------
 * Matrix.vector product y = A.x with no prior halo update of x.
 *
 * This function does not include a halo update of x prior to multiplication
 * by A, so it should be called only when the halo of x is known to already
 * be up to date (in which case we avoid the performance penalty of a
 * redundant update by using this variant of the matrix.vector product).
 *
 * parameters:
 *   matrix <-- Pointer to matrix structure
 *   x      <-- Multipliying vector values
 *   y      --> Resulting vector
 *----------------------------------------------------------------------------*/

void
cs_matrix_vector_multiply_nosync(const cs_matrix_t  *matrix,
                                 const cs_real_t    *x,
                                 cs_real_t          *restrict y)
{
  assert(matrix != NULL);

  /* Non-blocked version */

  if (matrix->b_size[3] == 1) {

    if (matrix->vector_multiply[0] != NULL)
      matrix->vector_multiply[0](false, matrix, x, y);
    else
      bft_error(__FILE__, __LINE__, 0,
                _("Matrix is missing a vector multiply function."));

  }

  /* Blocked version */

  else { /* if (matrix->b_size[3] > 1) */

    if (matrix->vector_multiply[2] != NULL)
      matrix->vector_multiply[2](false, matrix, x, y);
    else
      bft_error(__FILE__, __LINE__, 0,
                _("Block matrix is missing a vector multiply function."));

  }
}

/*----------------------------------------------------------------------------
 * Matrix.vector product y = (A-D).x
 *
 * This function includes a halo update of x prior to multiplication by A.
 *
 * parameters:
 *   rotation_mode <-- Halo update option for rotational periodicity
 *   matrix        <-- Pointer to matrix structure
 *   x             <-> Multipliying vector values (ghost values updated)
 *   y             --> Resulting vector
 *----------------------------------------------------------------------------*/

void
cs_matrix_exdiag_vector_multiply(cs_perio_rota_t     rotation_mode,
                                 const cs_matrix_t  *matrix,
                                 cs_real_t          *restrict x,
                                 cs_real_t          *restrict y)
{
  assert(matrix != NULL);

  if (matrix->halo != NULL)
    _pre_vector_multiply_sync(rotation_mode,
                              matrix,
                              x,
                              y);

  /* Non-blocked version */

  if (matrix->b_size[3] == 1) {
    if (matrix->vector_multiply[1] != NULL)
      matrix->vector_multiply[1](true, matrix, x, y);
    else
      bft_error(__FILE__, __LINE__, 0,
                _("Matrix is missing a vector multiply function."));
  }

  /* Blocked version */

  else {
    if (matrix->vector_multiply[3] != NULL)
      matrix->vector_multiply[3](false, matrix, x, y);
    else
      bft_error(__FILE__, __LINE__, 0,
                _("Block matrix is missing a vector multiply function."));
  }
}

/*----------------------------------------------------------------------------
 * Tune local matrix.vector product operations.
 *
 * parameters:
 *   t_measure      <-- minimum time for each measure
 *   sym_weight     <-- weight of symmetric case (0 <= weight <= 1)
 *   block_weight   <-- weight of block case (0 <= weight <= 1)
 *   n_min_spmv     <-- minimum number of SpMv products (to estimate
 *                      amortization of coefficients assignment)
 *   n_cells        <-- number of local cells
 *   n_cells_ext    <-- number of cells including ghost cells (array size)
 *   n_faces        <-- local number of internal faces
 *   cell_num       <-- global cell numbers (1 to n)
 *   face_cell      <-- face -> cells connectivity (1 to n)
 *   halo           <-- cell halo structure
 *   numbering      <-- vectorization or thread-related numbering info, or NULL
 *
 * returns:
 *   pointer to tuning results structure
 *----------------------------------------------------------------------------*/

cs_matrix_variant_t *
cs_matrix_variant_tuned(double                 t_measure,
                        double                 sym_weight,
                        double                 block_weight,
                        int                    n_min_products,
                        cs_lnum_t              n_cells,
                        cs_lnum_t              n_cells_ext,
                        cs_lnum_t              n_faces,
                        const cs_gnum_t       *cell_num,
                        const cs_lnum_t       *face_cell,
                        const cs_halo_t       *halo,
                        const cs_numbering_t  *numbering)
{
  int  t_id, t_id_max, v_id, sub_id, ed_flag;
  int  _sym_flag, _block_flag;

  double speedup, max_speedup;
  double t_speedup[CS_MATRIX_N_TYPES][8];
  double t_overhead[CS_MATRIX_N_TYPES][4];
  int cur_select[8];

  int  sym_flag = 0, block_flag = 0;
  int  n_variants = 0;
  cs_matrix_variant_t  *m_variant = NULL, *v = NULL;

  cs_matrix_variant_t  *r = NULL;

  /* Base flags on weights */

  if (sym_weight > 0.) {
    if (sym_weight < 1.)
      sym_flag = 2;
    else
      sym_flag = 1;
  }

  if (block_weight > 0.) {
    if (block_weight < 1.)
      block_flag = 2;
    else
      block_flag = 1;
  }

  for (t_id = 0; t_id < CS_MATRIX_N_TYPES; t_id++) {
    for (sub_id = 0; sub_id < 8; sub_id++)
      t_speedup[t_id][sub_id] = -1;
    for (sub_id = 0; sub_id < 4; sub_id++)
      t_overhead[t_id][sub_id] = 0;
  }

  /* Build variants array */
  /*----------------------*/

  _build_variant_list(sym_flag,
                      block_flag,
                      &n_variants,
                      &m_variant);

  /* Run tests on variants */

  _matrix_tune_test(t_measure,
                    n_variants,
                    n_cells,
                    n_cells_ext,
                    n_faces,
                    cell_num,
                    face_cell,
                    halo,
                    numbering,
                    m_variant);

  /* Print info on variants */

  _matrix_tune_create_assign_title(1, 0, 0);
  for (v_id = 0; v_id < n_variants; v_id++)
    _matrix_tune_create_assign_stats(m_variant, v_id, 1, 0, 0);

  for (_block_flag = 0; _block_flag < 2; _block_flag++) {
    if (   (_block_flag == 0 && block_flag == 1)
        || (_block_flag == 1 && block_flag == 0))
    continue;
    for (_sym_flag = 0; _sym_flag < 2; _sym_flag++) {
      if (   (_sym_flag == 0 && sym_flag == 1)
          || (_sym_flag == 1 && sym_flag == 0))
      continue;
      _matrix_tune_create_assign_title(0,
                                       _sym_flag,
                                       _block_flag);
      for (v_id = 0; v_id < n_variants; v_id++)
        _matrix_tune_create_assign_stats(m_variant,
                                         v_id,
                                         0,
                                         _sym_flag,
                                         _block_flag);
    }
  }

  for (_block_flag = 0; _block_flag < 2; _block_flag++) {
    if (   (_block_flag == 0 && block_flag == 1)
        || (_block_flag == 1 && block_flag == 0))
    continue;
    for (_sym_flag = 0; _sym_flag < 2; _sym_flag++) {
      if (   (_sym_flag == 0 && sym_flag == 1)
          || (_sym_flag == 1 && sym_flag == 0))
      continue;
      for (ed_flag = 0; ed_flag < 2; ed_flag++) {
        _matrix_tune_spmv_title(_sym_flag,
                                ed_flag,
                                _block_flag);
          for (v_id = 0; v_id < n_variants; v_id++)
            _matrix_tune_spmv_stats(m_variant,
                                    v_id,
                                    _sym_flag,
                                    ed_flag,
                                    _block_flag);
      }
    }
  }

  /* Select type of matrix with best possible performance */

  for (v_id = 0; v_id < n_variants; v_id++) {
    v = m_variant + v_id;
    for (_block_flag = 0; _block_flag < 2; _block_flag++) {
      for (_sym_flag = 0; _sym_flag < 2; _sym_flag++) {
        int o_id = _block_flag*2 + _sym_flag;
        if (   v->matrix_assign_cost[_block_flag*2 + _sym_flag] > 0
            && (n_min_products > 0 && n_min_products < 10000))
          t_overhead[v->type][o_id]
            = v->matrix_assign_cost[o_id] / n_min_products;
        sub_id = _block_flag*4 + _sym_flag*2;
        speedup = (  (  m_variant->matrix_vector_cost[sub_id]
                      + t_overhead[m_variant->type][o_id])
                   / (  v->matrix_vector_cost[sub_id]
                      + t_overhead[v->type][o_id]));
      if (t_speedup[v->type][sub_id] < speedup)
        t_speedup[v->type][sub_id] = speedup;
      }
    }
  }

  max_speedup = 0;
  t_id_max = 0;

  for (t_id = 0; t_id < CS_MATRIX_N_TYPES; t_id++) {
    speedup = (1.0-block_weight) * (1.0-sym_weight) * t_speedup[t_id][0];
    speedup += (1.0 - block_weight) * sym_weight * t_speedup[t_id][2];
    speedup += block_weight * (1.0-sym_weight) * t_speedup[t_id][4];
    speedup += block_weight * sym_weight * t_speedup[t_id][6];
    if (block_weight < 1.) {
      if (sym_weight < 1. && t_speedup[t_id][0] < 0)
        speedup = -1;
      if (sym_weight > 0. && t_speedup[t_id][2] < 0)
        speedup = -1;
    }
    if (block_weight > 0.) {
      if (sym_weight < 1. && t_speedup[t_id][4] < 0)
        speedup = -1;
      if (sym_weight > 0. && t_speedup[t_id][6] < 0)
        speedup = -1;
    }
    if (speedup > max_speedup) {
      max_speedup = speedup;
      t_id_max = t_id;
    }
  }

  /* Now that the best type is chosen, build the variant */

  BFT_MALLOC(r, 1, cs_matrix_variant_t);

  _variant_init(r);

  strncpy(r->name, cs_matrix_type_name[t_id_max], 31);
  r->type = t_id_max;
  r->symmetry = sym_flag;

  for (sub_id = 0; sub_id < 8; sub_id++)
    cur_select[sub_id] = -1;

  for (v_id = 0; v_id < n_variants; v_id++) {

    v = m_variant + v_id;
    if (v->type != r->type)
      continue;

    if (v->matrix_create_cost > 0)
      r->matrix_create_cost = v->matrix_create_cost;
    for (sub_id = 0; sub_id < 4; sub_id++) {
      if (v->matrix_assign_cost[sub_id] > 0)
        r->matrix_assign_cost[sub_id] = v->matrix_assign_cost[sub_id];
    }

    for (_block_flag = 1; _block_flag >= 0; _block_flag--) {
      for (_sym_flag = 1; _sym_flag >= 0; _sym_flag--) { /* non sym priority */
        for (ed_flag = 1; ed_flag >= 0; ed_flag--) { /* full matrix priority */

          sub_id = _block_flag*4 + _sym_flag*2 + ed_flag;

          if (v->matrix_vector_cost[sub_id] > 0) {
            if (   v->matrix_vector_cost[sub_id] < r->matrix_vector_cost[sub_id]
                || r->matrix_vector_cost[sub_id] < 0) {
              r->vector_multiply[_block_flag*2 + ed_flag]
                = v->vector_multiply[_block_flag*2 + ed_flag];
              r->matrix_vector_cost[sub_id] = v->matrix_vector_cost[sub_id];
              r->loop_length = v->loop_length;
              cur_select[sub_id] = v_id;
            }
          }

        }
      }
    }

  } /* End of loop on variants */

  /* print info on selected variants */

  cs_log_printf(CS_LOG_PERFORMANCE,
                "\n"
                "Selected matrix operation implementations:\n"
                "------------------------------------------\n");

#if defined(HAVE_MPI)

  if (cs_glob_n_ranks > 1) {

    int *select_loc, *select_sum;

    BFT_MALLOC(select_sum, n_variants*8, int);
    BFT_MALLOC(select_loc, n_variants*8, int);

    for (v_id = 0; v_id < n_variants; v_id++) {
      for (sub_id = 0; sub_id < 8; sub_id++)
        select_loc[v_id*8 + sub_id] = 0;
    }
    for (sub_id = 0; sub_id < 8; sub_id++) {
      if (cur_select[sub_id] > -1)
        select_loc[cur_select[sub_id]*8 + sub_id] = 1;
    }

    MPI_Allreduce(select_loc, select_sum, n_variants*8, MPI_INT, MPI_SUM,
                  cs_glob_mpi_comm);

    BFT_FREE(select_loc);

    for (_block_flag = 0; _block_flag < 2; _block_flag++) {
      for (_sym_flag = 0; _sym_flag < 2; _sym_flag++) {
        for (ed_flag = 0; ed_flag < 2; ed_flag++) {

          int count_tot = 0;

          sub_id = _block_flag*4 + _sym_flag*2 + ed_flag;

          for (v_id = 0; v_id < n_variants; v_id++)
            count_tot += (select_sum[v_id*8 + sub_id]);

          if (count_tot > 0) {
            cs_log_printf(CS_LOG_PERFORMANCE,
                          _("\n  -%s:\n"),
                          _(_matrix_operation_name[sub_id]));
            for (v_id = 0; v_id < n_variants; v_id++) {
              int scount = select_sum[v_id*8 + sub_id];
              if (scount > 0) {
                char title[36] =  {""};
                v = m_variant + v_id;
                cs_log_strpad(title, _(v->name), 32, 36);
                cs_log_printf(CS_LOG_PERFORMANCE,
                              _("    %s : %d ranks\n"), title, scount);
              }
            }
          }

        }
      }
    }

    BFT_FREE(select_sum);

  } /* if (cs_glob_n_ranks > 1) */

#endif

  if (cs_glob_n_ranks == 1) {

    cs_log_printf(CS_LOG_PERFORMANCE, "\n");

    for (_block_flag = 0; _block_flag < 2; _block_flag++) {
      for (_sym_flag = 0; _sym_flag < 2; _sym_flag++) {
        for (ed_flag = 0; ed_flag < 2; ed_flag++) {

          sub_id = _block_flag*4 + _sym_flag*2 + ed_flag;

          v_id = cur_select[sub_id];
          if (v_id > -1) {
            v = m_variant + v_id;
            cs_log_printf(CS_LOG_PERFORMANCE,
                          _("  %-44s : %s\n"),
                          _(_matrix_operation_name[sub_id]), _(v->name));
          }

        }
      }
    }

  } /* if (cs_glob_n_ranks == 1) */

  BFT_FREE(m_variant);

  return r;
}

/*----------------------------------------------------------------------------
 * Destroy a matrix variant structure.
 *
 * parameters:
 *   mv <-> Pointer to matrix variant pointer
 *----------------------------------------------------------------------------*/

void
cs_matrix_variant_destroy(cs_matrix_variant_t  **mv)
{
  if (mv != NULL)
    BFT_FREE(*mv);
}

/*----------------------------------------------------------------------------
 * Get the type associated with a matrix variant.
 *
 * parameters:
 *   mv <-- Pointer to matrix variant structure
 *----------------------------------------------------------------------------*/

cs_matrix_type_t
cs_matrix_variant_type(const cs_matrix_variant_t  *mv)
{
  return mv->type;
}

/*----------------------------------------------------------------------------
 * Test local matrix.vector product operations.
 *
 * parameters:
 *   n_cells        <-- number of local cells
 *   n_cells_ext    <-- number of cells including ghost cells (array size)
 *   n_faces        <-- local number of internal faces
 *   cell_num       <-- global cell numbers (1 to n)
 *   face_cell      <-- face -> cells connectivity (1 to n)
 *   halo           <-- cell halo structure
 *   numbering      <-- vectorization or thread-related numbering info, or NULL
 *
 * returns:
 *   pointer to tuning results structure
 *----------------------------------------------------------------------------*/

void
cs_matrix_variant_test(cs_lnum_t              n_cells,
                       cs_lnum_t              n_cells_ext,
                       cs_lnum_t              n_faces,
                       const cs_gnum_t       *cell_num,
                       const cs_lnum_t       *face_cell,
                       const cs_halo_t       *halo,
                       const cs_numbering_t  *numbering)
{
  int  sym_flag, block_flag;
  int  n_variants = 0;
  cs_matrix_variant_t  *m_variant = NULL;

  /* Test basic flag combinations */

  bft_printf
    (_("\n"
       "Checking matrix structure and operation variants (diff/reference):\n"
       "------------------------------------------------\n\n"));

  for (sym_flag = 0; sym_flag < 2; sym_flag++) {

    for (block_flag = 0; block_flag < 2; block_flag++) {

      /* Build variants array */

      _build_variant_list(sym_flag,
                          block_flag,
                          &n_variants,
                          &m_variant);

      /* Run tests on variants */

      _matrix_check(n_variants,
                    n_cells,
                    n_cells_ext,
                    n_faces,
                    cell_num,
                    face_cell,
                    halo,
                    numbering,
                    m_variant);

      n_variants = 0;
      BFT_FREE(m_variant);

    }

  }
}

/*----------------------------------------------------------------------------*/

END_C_DECLS
