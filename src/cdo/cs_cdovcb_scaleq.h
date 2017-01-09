#ifndef __CS_CDOVCB_SCALEQ_H__
#define __CS_CDOVCB_SCALEQ_H__

/*============================================================================
 * Build an algebraic CDO vertex+cell-based system for scalar conv./diff. eq.
 *============================================================================*/

/*
  This file is part of Code_Saturne, a general-purpose CFD tool.

  Copyright (C) 1998-2017 EDF S.A.

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

/*----------------------------------------------------------------------------
 *  Local headers
 *----------------------------------------------------------------------------*/

#include "cs_base.h"
#include "cs_time_step.h"
#include "cs_mesh.h"
#include "cs_field.h"
#include "cs_cdo_connect.h"
#include "cs_cdo_quantities.h"
#include "cs_equation_param.h"
#include "cs_source_term.h"

/*----------------------------------------------------------------------------*/

BEGIN_C_DECLS

/*============================================================================
 * Macro definitions
 *============================================================================*/

/*============================================================================
 * Type definitions
 *============================================================================*/

/* Algebraic system for CDO vertex-based discretization */
typedef struct _cs_cdovcb_scaleq_t cs_cdovcb_scaleq_t;

/*============================================================================
 * Public function prototypes
 *============================================================================*/

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Set shared pointers from the main domain members
 *
 * \param[in]  quant       additional mesh quantities struct.
 * \param[in]  connect     pointer to a cs_cdo_connect_t struct.
 * \param[in]  time_step   pointer to a time step structure
 */
/*----------------------------------------------------------------------------*/

void
cs_cdovcb_scaleq_set_shared_pointers(const cs_cdo_quantities_t    *quant,
                                     const cs_cdo_connect_t       *connect,
                                     const cs_time_step_t         *time_step);

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Allocate work buffer and general structures related to CDO
 *         vertex+cell-based schemes
 */
/*----------------------------------------------------------------------------*/

void
cs_cdovcb_scaleq_initialize(void);

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Free buffers and generic structures related to CDO vertex+cell-based
 *         schemes
 */
/*----------------------------------------------------------------------------*/

void
cs_cdovcb_scaleq_finalize(void);

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Initialize a cs_cdovcb_scaleq_t structure
 *
 * \param[in] eqp       pointer to a cs_equation_param_t structure
 * \param[in] mesh      pointer to a cs_mesh_t structure
 *
 * \return a pointer to a new allocated cs_cdovcb_scaleq_t structure
 */
/*----------------------------------------------------------------------------*/

void  *
cs_cdovcb_scaleq_init(const cs_equation_param_t   *eqp,
                      const cs_mesh_t             *mesh);

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Destroy a cs_cdovcb_scaleq_t structure
 *
 * \param[in, out]  builder   pointer to a cs_cdovcb_scaleq_t structure
 *
 * \return a NULL pointer
 */
/*----------------------------------------------------------------------------*/

void *
cs_cdovcb_scaleq_free(void   *builder);

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Display information related to the monitoring of the current system
 *
 * \param[in]  eqname    name of the related equation
 * \param[in]  builder   pointer to a cs_cdovcb_scaleq_t structure
 */
/*----------------------------------------------------------------------------*/

void
cs_cdovcb_scaleq_monitor(const char   *eqname,
                         const void   *builder);

/*----------------------------------------------------------------------------*/
/*!
 * \brief   Compute the contributions of source terms (store inside builder)
 *
 * \param[in, out] builder     pointer to a cs_cdovcb_scaleq_t structure
 */
/*----------------------------------------------------------------------------*/

void
cs_cdovcb_scaleq_compute_source(void            *builder);

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Allocate the matrix related to the algebraic system to solve
 *
 * \return  a pointer to a new allocated structure
 */
/*----------------------------------------------------------------------------*/

cs_matrix_t *
cs_cdovcb_allocate_matrix(void);

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Allocate and initialize the right-hand side associated to the given
 *         builder structure
 *
 * \param[in, out] builder    pointer to generic builder structure
 *
 * \return an initialized array
 */
/*----------------------------------------------------------------------------*/

cs_real_t *
cs_cdovcb_initialize_rhs(void       *builder);

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Build the linear system arising from a scalar convection/diffusion
 *         equation with a CDO vertex+cell-based scheme.
 *         One works cellwise and then process to the assembly
 *
 * \param[in]      mesh       pointer to a cs_mesh_t structure
 * \param[in]      field_val  pointer to the current value of the vertex field
 * \param[in]      dt_cur     current value of the time step
 * \param[in, out] builder    pointer to cs_cdovcb_scaleq_t structure
 * \param[in, out] rhs        right-hand side
 * \param[in, out] matrix     pointer to cs_matrix_t structure to compute
 */
/*----------------------------------------------------------------------------*/

void
cs_cdovcb_scaleq_build_system(const cs_mesh_t       *mesh,
                              const cs_real_t       *field_val,
                              double                 dt_cur,
                              void                  *builder,
                              cs_real_t             *rhs,
                              cs_matrix_t           *matrix);

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Store solution(s) of the linear system into a field structure
 *         Update extra-field values if required (for hybrid discretization)
 *
 * \param[in]      solu       solution array
 * \param[in]      rhs        rhs associated to this solution array
 * \param[in, out] builder    pointer to builder structure
 * \param[in, out] field_val  pointer to the current value of the field
 */
/*----------------------------------------------------------------------------*/

void
cs_cdovcb_scaleq_update_field(const cs_real_t     *solu,
                              const cs_real_t     *rhs,
                              void                *builder,
                              cs_real_t           *field_val);

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Get the computed values at cell centers (DoF used in the linear
 *         system are located at primal vertices and field related to the
 *         structure equation is also attached to primal vertices
 *
 * \param[in]  builder    pointer to a builder structure
 *
 * \return  a pointer to an array of double
 */
/*----------------------------------------------------------------------------*/

double *
cs_cdovcb_scaleq_get_cell_values(const void          *builder);

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Compute the diffusive and convective flux across a list of faces
 *
 * \param[in]       direction  indicate in which direction flux is > 0
 * \param[in]       pdi        pointer to an array of field values
 * \param[in]       ml_id      id related to a cs_mesh_location_t struct.
 * \param[in, out]  builder    pointer to a builder structure
 * \param[in, out]  diff_flux  pointer to the value of the diffusive flux
 * \param[in, out]  conv_flux  pointer to the value of the convective flux
 */
/*----------------------------------------------------------------------------*/

void
cs_cdovcb_scaleq_compute_flux_across_plane(const cs_real_t     direction[],
                                           const cs_real_t    *pdi,
                                           int                 ml_id,
                                           void               *builder,
                                           double             *diff_flux,
                                           double             *conv_flux);

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Cellwise computation of the diffusive flux across all dual faces.
 *
 * \param[in]       values      discrete values for the potential
 * \param[in, out]  builder     pointer to builder structure
 * \param[in, out]  diff_flux   value of the diffusive flux
  */
/*----------------------------------------------------------------------------*/

void
cs_cdovcb_scaleq_cellwise_diff_flux(const cs_real_t   *values,
                                    void              *builder,
                                    cs_real_t         *diff_flux);

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Predefined extra-operations related to this equation
 *
 * \param[in]       eqname     name of the equation
 * \param[in]       field      pointer to a field structure
 * \param[in, out]  builder    pointer to builder structure
 */
/*----------------------------------------------------------------------------*/

void
cs_cdovcb_scaleq_extra_op(const char            *eqname,
                          const cs_field_t      *field,
                          void                  *builder);

/*----------------------------------------------------------------------------*/

END_C_DECLS

#endif /* __CS_CDOVCB_SCALEQ_H__ */
