/*============================================================================
 * Mass source terms computation.
 *============================================================================*/

/*
  This file is part of Code_Saturne, a general-purpose CFD tool.

  Copyright (C) 1998-2020 EDF S.A.

  This program is free software; you can redistribute it and/or modify it under
  the terms of the GNU General Public License as published by the Free Software
  Foundation; either version 2 of the License, or (at your option) any later
  version.

  This program is distributed in the hope that it will be useful, but WITHOUT
  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
  details.

  You should have received a copy of the GNU General Public License along with
  this program; if not, write to the Free Software Foundation, Inc., 51 Franklin
  Street, Fifth Floor, Boston, MA 02110-1301, USA.
*/

/*----------------------------------------------------------------------------*/

#include "cs_defs.h"

/*----------------------------------------------------------------------------
 * Standard C library headers
 *----------------------------------------------------------------------------*/

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/*----------------------------------------------------------------------------
 * Local headers
 *----------------------------------------------------------------------------*/

#include "cs_base.h"
#include "cs_field.h"
#include "cs_math.h"
#include "cs_mesh.h"
#include "cs_mesh_quantities.h"

/*----------------------------------------------------------------------------
 * Header for the current file
 *----------------------------------------------------------------------------*/

#include "cs_mass_source_terms.h"

/*----------------------------------------------------------------------------*/

BEGIN_C_DECLS

/*=============================================================================
 * Additional doxygen documentation
 *============================================================================*/

/*!
  \file cs_mass_source_terms.c
        Mass source terms computation.

*/

/*! \cond DOXYGEN_SHOULD_SKIP_THIS */

/*============================================================================
 * Private function definitions
 *============================================================================*/

/*! (DOXYGEN_SHOULD_SKIP_THIS) \endcond */

/*============================================================================
 * Public function definitions
 *============================================================================*/

/*----------------------------------------------------------------------------*/
/*!
 * \brief Implicit and explicit mass source terms computation.
 *
 * \param[in]     ncesmp        number of cells with mass source term
 * \param[in]     iterns        iteration number on Navier-Stoke
 * \param[in]     isnexp        sources terms of treated phasis extrapolation
 *                              indicator
 * \param[in]     icetsm        source mass cells pointer (1-based numbering)
 * \param[in]     itpsmp        mass source type for the working variable
 *                              (see \ref cs_user_mass_source_terms)
 * \param[in]     volume        cells volume
 * \param[in]     pvara         variable value at time step beginning
 * \param[in]     smcelp        value of the variable associated with mass source
 * \param[in]     gamma         flow mass value
 * \param[in,out] tsexp         explicit source term part linear in the variable
 * \param[in,out] tsimp         associated value with \c tsexp
 *                              to be stored in the matrix
 * \param[out]    gapinj        explicit source term part independant
 *                              of the variable
 */
/*----------------------------------------------------------------------------*/

void
cs_mass_source_terms(cs_lnum_t             ncesmp,
                     int                   iterns,
                     int                   isnexp,
                     const int             icetsm[],
                     int                   itpsmp[],
                     const cs_real_t       volume[],
                     const cs_real_t       pvara[],
                     const cs_real_t       smcelp[],
                     const cs_real_t       gamma[],
                     cs_real_t             st_exp[],
                     cs_real_t             st_imp[],
                     cs_real_t             gapinj[])
{
  const cs_mesh_t *m = cs_glob_mesh;
  const cs_lnum_t n_cells = m->n_cells;

  /* Remark for tests on gamma[i] > O && itpsmp[i] == 1 :
     *
     * If we remove matter or enter with the cell value
     * then the equation on the variable has not been modified.
     *
     * Otherwise, we add the term gamma*(f_i-f^(n+1))
     *
     * In st_imp, we add the term that will go on the diagonal, which is Gamma.
     * In st_exp, we add the term for the right-hand side, which is
     *   Gamma * Pvar (avec Pvar)
     *
     * In gapinj, we place the term Gamma Pinj which will go to the right-hand side.
     *
     * The distinction between st_exp and W1 (which both go finally to the
     * right-hand side) is used for the 2nd-order time scheme. */

  if (iterns == 1) {
    for (cs_lnum_t i = 0; i < n_cells; i++) {
      gapinj[i] = 0.;
    }
    for (cs_lnum_t i = 0; i < ncesmp; i++) {
      cs_lnum_t c_id = icetsm[i] - 1;
      if (gamma[i] > 0. && itpsmp[i] == 1) {
        st_exp[c_id] = st_exp[c_id] - volume[c_id]*gamma[i] * pvara[c_id];
        gapinj[c_id] = volume[c_id]*gamma[i] * smcelp[i];
      }
    }
  }

  /* On the diagonal */

  if (isnexp > 0) {
    for (cs_lnum_t i = 0; i < ncesmp; i++) {
      cs_lnum_t c_id = icetsm[i] - 1;
      if (gamma[i] > 0. && itpsmp[i] == 1) {
        st_imp[c_id] = st_imp[c_id] + volume[c_id]*gamma[i];
      }
    }
  }
  else {
    for (cs_lnum_t i = 0; i < ncesmp; i++) {
      cs_lnum_t c_id = icetsm[i] - 1;
      if (gamma[i] > 0. && itpsmp[i] == 1) {
        st_imp[c_id] = st_imp[c_id] + volume[c_id]*gamma[i];
      }
    }
  }

}

/*----------------------------------------------------------------------------*/

END_C_DECLS
