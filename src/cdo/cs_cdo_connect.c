/*============================================================================
 * Manage connectivity (Topological features of the mesh)
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

/*----------------------------------------------------------------------------*/

#include "cs_defs.h"

/*----------------------------------------------------------------------------
 * Standard C library headers
 *----------------------------------------------------------------------------*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/*----------------------------------------------------------------------------
 * Local headers
 *----------------------------------------------------------------------------*/

#include "bft_mem.h"

#include "cs_cdo.h"
#include "cs_log.h"
#include "cs_order.h"
#include "cs_parall.h"
#include "cs_sort.h"


/*----------------------------------------------------------------------------
 * Header for the current file
 *----------------------------------------------------------------------------*/

#include "cs_cdo_connect.h"

/*----------------------------------------------------------------------------*/

BEGIN_C_DECLS

/*=============================================================================
 * Local macro and structure definitions
 *============================================================================*/

#define CS_CDO_CONNECT_DBG 0

/* Temporary structure to build edge/vertices connectivities */
typedef struct {

  cs_lnum_t  n_vertices;
  cs_lnum_t  n_edges;

  int  *e2v_lst;  /* Edge ref. definition (2*n_edges) */
  int  *v2v_idx;
  int  *v2v_lst;
  int  *v2v_edge_lst;

} _edge_builder_t;

/*============================================================================
 * Private function prototypes
 *============================================================================*/

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Add a entry in the face --> edges connectivity
 *
 * \param[in]      shift     position where to add the new entry
 * \param[in]      v1_num    number of the first vertex
 * \param[in]      v2_num    number of the second vertex
 * \param[in]      builder   pointer to a _edge_builder_t structure
 * \param[in, out] f2e       face --> edges connectivity
 */
/*----------------------------------------------------------------------------*/

static void
_add_f2e_entry(cs_lnum_t                   shift,
               cs_lnum_t                   v1_num,
               cs_lnum_t                   v2_num,
               const _edge_builder_t      *builder,
               cs_sla_matrix_t            *f2e)
{
  cs_lnum_t  i;

  /* Sanity check */
  assert(v1_num > 0);
  assert(v2_num > 0);
  assert(builder != NULL);
  assert(builder->v2v_idx != NULL);
  assert(builder->v2v_idx[v1_num] > builder->v2v_idx[v1_num-1]);

  /* Get edge number */
  cs_lnum_t  edge_sgn_num = 0;
  cs_lnum_t  *v2v_idx = builder->v2v_idx, *v2v_lst = builder->v2v_lst;

  for (i = v2v_idx[v1_num-1]; i < v2v_idx[v1_num]; i++) {
    if (v2v_lst[i] == v2_num) {
      edge_sgn_num = builder->v2v_edge_lst[i];
      break;
    }
  }

  if (edge_sgn_num == 0)
    bft_error(__FILE__, __LINE__, 0,
              _(" The given couple of vertices (number): [%d, %d]\n"
                " is not defined in the edge structure.\n"), v1_num, v2_num);

  f2e->col_id[shift] = CS_ABS(edge_sgn_num) - 1;
  if (edge_sgn_num < 0)
    f2e->sgn[shift] = -1;
  else
    f2e->sgn[shift] = 1;

}

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Define the face -> edges connectivity which is stored in a
 *         cs_sla_matrix_t structure
 *
 * \param[in]  m         pointer to a cs_mesh_t structure
 * \param[in]  builder   pointer to the _edge_builder_t structure
 *
 * \return a pointer to a new allocated cs_sla_matrix_t structure
 */
/*----------------------------------------------------------------------------*/

static cs_sla_matrix_t *
_build_f2e_connect(const cs_mesh_t         *m,
                   const _edge_builder_t   *builder)
{
  int  i, j, s, e, shift, v1_num, v2_num;

  cs_sla_matrix_t  *f2e = NULL;

  const int  n_i_faces = m->n_i_faces;
  const int  n_b_faces = m->n_b_faces;
  const int  n_faces = n_i_faces + n_b_faces;
  const int  n_edges = builder->n_edges;

  f2e = cs_sla_matrix_create(n_faces, n_edges, 1, CS_SLA_MAT_DEC, false);

  /* Build index */
  for (i = 0; i < n_i_faces; i++)
    f2e->idx[i+1] += m->i_face_vtx_idx[i+1] - m->i_face_vtx_idx[i];
  for (i = 0, j=n_i_faces+1; i < n_b_faces; i++, j++)
    f2e->idx[j] += m->b_face_vtx_idx[i+1] - m->b_face_vtx_idx[i];
  for (i = 0; i < n_faces; i++)
    f2e->idx[i+1] += f2e->idx[i];

  assert(f2e->idx[n_faces]
         == m->i_face_vtx_idx[n_i_faces] + m->b_face_vtx_idx[n_b_faces]);

  /* Build matrix */
  BFT_MALLOC(f2e->col_id, f2e->idx[n_faces], cs_lnum_t);
  BFT_MALLOC(f2e->sgn, f2e->idx[n_faces], short int);

  /* Border faces */
  for (i = 0; i < n_b_faces; i++) {

    s = m->b_face_vtx_idx[i], e = m->b_face_vtx_idx[i+1];

    cs_lnum_t  f_id = n_i_faces + i;

    shift = f2e->idx[f_id];
    v1_num = m->b_face_vtx_lst[e-1] + 1;
    v2_num = m->b_face_vtx_lst[s] + 1;
    _add_f2e_entry(shift, v1_num, v2_num, builder, f2e);

    for (j = s; j < e-1; j++) {

      shift++;
      v1_num = m->b_face_vtx_lst[j] + 1;
      v2_num = m->b_face_vtx_lst[j+1] + 1;
      _add_f2e_entry(shift, v1_num, v2_num, builder, f2e);

    }

  } /* End of loop on border faces */

  for (cs_lnum_t f_id = 0; f_id < n_i_faces; f_id++) {

    s = m->i_face_vtx_idx[f_id], e = m->i_face_vtx_idx[f_id+1];
    shift = f2e->idx[f_id];
    v1_num = m->i_face_vtx_lst[e-1] + 1;
    v2_num = m->i_face_vtx_lst[s] + 1;
    _add_f2e_entry(shift, v1_num, v2_num, builder, f2e);

    for (j = s; j < e-1; j++) {

      shift++;
      v1_num = m->i_face_vtx_lst[j] + 1;
      v2_num = m->i_face_vtx_lst[j+1] + 1;
      _add_f2e_entry(shift, v1_num, v2_num, builder, f2e);

    }

  } /* End of loop on internal faces */

  return f2e;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Define the edge -> vertices connectivity which is stored in a
 *         cs_sla_matrix_t structure
 *
 * \param[in]  builder   pointer to the _edge_builder_t structure
 *
 * \return a pointer to a new allocated cs_sla_matrix_t structure
 */
/*----------------------------------------------------------------------------*/

static cs_sla_matrix_t *
_build_e2v_connect(const _edge_builder_t  *builder)
{
  int  i;

  cs_sla_matrix_t  *e2v = NULL;

  const int  n_vertices = builder->n_vertices;
  const int  n_edges = builder->n_edges;

  e2v = cs_sla_matrix_create(n_edges, n_vertices, 1, CS_SLA_MAT_DEC, false);

  /* Build index */
  e2v->idx[0] = 0;
  for (i = 0; i < n_edges; i++)
    e2v->idx[i+1] = e2v->idx[i] + 2;

  assert(e2v->idx[n_edges] == 2*n_edges);

  /* Build matrix */
  BFT_MALLOC(e2v->col_id, e2v->idx[n_edges], cs_lnum_t);
  BFT_MALLOC(e2v->sgn, e2v->idx[n_edges], short int);

  for (i = 0; i < n_edges; i++) {

    e2v->col_id[2*i] = builder->e2v_lst[2*i] - 1;
    e2v->sgn[2*i] = -1;
    e2v->col_id[2*i+1] = builder->e2v_lst[2*i+1] - 1;
    e2v->sgn[2*i+1] = 1;

  }

  return e2v;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Allocate and define a _edge_builder_t structure
 *
 * \param[in]  m   pointer to the cs_mesh_t structure
 *
 * \return a pointer to a new allocated _edge_builder_t structure
 */
/*----------------------------------------------------------------------------*/

static _edge_builder_t *
_create_edge_builder(const cs_mesh_t  *m)
{
  int  i, j, k, v1, v2, o1, o2, s, e, nfv, shift, s1, s2;

  int  n_edges = 0, n_init_edges = 0;
  int  n_max_face_vertices = 0;
  cs_lnum_t  *f_vertices = NULL, *vtx_shift = NULL;
  cs_lnum_t  *v2v_idx = NULL, *v2v_lst = NULL, *v2e_lst = NULL;
  int  *e2v_ref_lst = NULL;
  cs_gnum_t  *e2v_lst = NULL; /* Only because we have to use cs_order */
  cs_lnum_t  *order = NULL;

  _edge_builder_t  *builder = NULL;

  const int n_vertices = m->n_vertices;
  const int n_i_faces = m->n_i_faces;
  const int n_b_faces = m->n_b_faces;

  /* Compute max. number of vertices by face */
  for (i = 0; i < n_b_faces; i++)
    n_max_face_vertices = CS_MAX(n_max_face_vertices,
                                 m->b_face_vtx_idx[i+1] - m->b_face_vtx_idx[i]);
  for (i = 0; i < n_i_faces; i++)
    n_max_face_vertices = CS_MAX(n_max_face_vertices,
                                 m->i_face_vtx_idx[i+1] - m->i_face_vtx_idx[i]);

  BFT_MALLOC(f_vertices, n_max_face_vertices + 1, cs_lnum_t);

  n_init_edges = m->b_face_vtx_idx[n_b_faces];
  n_init_edges += m->i_face_vtx_idx[n_i_faces];

  /* Build e2v_lst */
  BFT_MALLOC(e2v_lst, 2*n_init_edges, cs_gnum_t);

  shift = 0;
  for (i = 0; i < n_b_faces; i++) {

    s = m->b_face_vtx_idx[i], e = m->b_face_vtx_idx[i+1];
    nfv = e - s;

    for (j = s, k = 0; j < e; j++, k++)
      f_vertices[k] = m->b_face_vtx_lst[j] + 1;
    f_vertices[nfv] = m->b_face_vtx_lst[s] + 1;

    for (k = 0; k < nfv; k++) {

      v1 = f_vertices[k], v2 = f_vertices[k+1];
      if (v1 < v2)
        e2v_lst[2*shift] = v1, e2v_lst[2*shift+1] = v2;
      else
        e2v_lst[2*shift] = v2, e2v_lst[2*shift+1] = v1;
      shift++;

    }

  } /* End of loop on border faces */

  for (i = 0; i < n_i_faces; i++) {

    s = m->i_face_vtx_idx[i], e = m->i_face_vtx_idx[i+1];
    nfv = e - s;

    for (j = s, k = 0; j < e; j++, k++)
      f_vertices[k] = m->i_face_vtx_lst[j] + 1;
    f_vertices[nfv] = m->i_face_vtx_lst[s] + 1;

    for (k = 0; k < nfv; k++) {

      v1 = f_vertices[k], v2 = f_vertices[k+1];
      if (v1 < v2)
        e2v_lst[2*shift] = v1, e2v_lst[2*shift+1] = v2;
      else
        e2v_lst[2*shift] = v2, e2v_lst[2*shift+1] = v1;
      shift++;

    }

  } /* End of loop on interior faces */

  assert(shift == n_init_edges);

  BFT_MALLOC(order, n_init_edges, cs_lnum_t);
  cs_order_gnum_allocated_s(NULL, e2v_lst, 2, order, n_init_edges);

  BFT_MALLOC(v2v_idx, n_vertices + 1, int);
  for (i = 0; i < n_vertices + 1; i++)
    v2v_idx[i] = 0;

  if (n_init_edges > 0) {

    BFT_MALLOC(e2v_ref_lst, 2*n_init_edges, int);

    o1 = order[0];
    v1 = e2v_lst[2*o1];
    v2 = e2v_lst[2*o1+1];

    e2v_ref_lst[0] = v1;
    e2v_ref_lst[1] = v2;
    v2v_idx[v1] += 1;
    v2v_idx[v2] += 1;
    shift = 1;

    for (i = 1; i < n_init_edges; i++) {

      o1 = order[i-1];
      o2 = order[i];

      if (   e2v_lst[2*o1]   != e2v_lst[2*o2]
          || e2v_lst[2*o1+1] != e2v_lst[2*o2+1]) {

        v2v_idx[e2v_lst[2*o2]] += 1;
        v2v_idx[e2v_lst[2*o2+1]] += 1;
        e2v_ref_lst[2*shift] = e2v_lst[2*o2];
        e2v_ref_lst[2*shift+1] = e2v_lst[2*o2+1];
        shift++;

      }

    } /* End of loop on edges */

  } /* n_init_edges > 0 */

  n_edges = shift;

  for (i = 0; i < n_vertices; i++)
    v2v_idx[i+1] += v2v_idx[i];

  /* Free memory */
  BFT_FREE(e2v_lst);
  BFT_FREE(order);
  BFT_FREE(f_vertices);

  if (n_edges > 0) {

    BFT_MALLOC(v2v_lst, v2v_idx[n_vertices], cs_lnum_t);
    BFT_MALLOC(v2e_lst, v2v_idx[n_vertices], cs_lnum_t);
    BFT_MALLOC(vtx_shift, n_vertices, int);

    for (i = 0; i < n_vertices; i++)
      vtx_shift[i] = 0;

    for (i = 0; i < n_edges; i++) {

      v1 = e2v_ref_lst[2*i] - 1;
      v2 = e2v_ref_lst[2*i+1] - 1;
      s1 = v2v_idx[v1] + vtx_shift[v1];
      s2 = v2v_idx[v2] + vtx_shift[v2];
      vtx_shift[v1] += 1;
      vtx_shift[v2] += 1;
      v2v_lst[s1] = v2 + 1;
      v2v_lst[s2] = v1 + 1;

      if (v1 < v2)
        v2e_lst[s1] = i+1, v2e_lst[s2] = -(i+1);
      else
        v2e_lst[s1] = -(i+1), v2e_lst[s2] = i+1;

    } /* End of loop on edges */

    BFT_FREE(vtx_shift);

  } /* n_edges > 0 */

  /* Return pointers */
  BFT_MALLOC(builder, 1, _edge_builder_t);

  builder->n_vertices = n_vertices;
  builder->n_edges = n_edges;
  builder->e2v_lst = e2v_ref_lst;
  builder->v2v_idx = v2v_idx;
  builder->v2v_lst = v2v_lst;
  builder->v2v_edge_lst = v2e_lst;

  return builder;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Destroy a _edge_builder structure
 *
 * \param[in]  p_builder   pointer to the _edge_builder structure pointer
 */
/*----------------------------------------------------------------------------*/

static void
_free_edge_builder(_edge_builder_t  **p_builder)
{
  _edge_builder_t  *_builder = *p_builder;

  if (_builder == NULL)
    return;

  BFT_FREE(_builder->e2v_lst);
  BFT_FREE(_builder->v2v_idx);
  BFT_FREE(_builder->v2v_lst);
  BFT_FREE(_builder->v2v_edge_lst);

  BFT_FREE(_builder);

  *p_builder = NULL;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Define the cell -> faces connectivity which is stored in a
 *         cs_sla_matrix_t structure
 *
 * \param[in]  mesh      pointer to a cs_mesh_t structure
 *
 * \return a pointer to a new allocated cs_sla_matrix_t structure
 */
/*----------------------------------------------------------------------------*/

static cs_sla_matrix_t *
_build_c2f_connect(const cs_mesh_t   *mesh)
{
  int  idx_size = 0;
  int  *cell_shift = NULL;
  cs_sla_matrix_t  *c2f = NULL;

  const int  n_cells = mesh->n_cells;
  const int  n_i_faces = mesh->n_i_faces;
  const int  n_b_faces = mesh->n_b_faces;
  const int  n_faces = n_i_faces + n_b_faces;

  c2f = cs_sla_matrix_create(n_cells, n_faces, 1, CS_SLA_MAT_DEC, false);

  BFT_MALLOC(cell_shift, n_cells, int);
# pragma omp parallel for if (n_cells > CS_THR_MIN)
  for (cs_lnum_t i = 0; i < n_cells; i++)
    cell_shift[i] = 0;

  for (cs_lnum_t i = 0; i < n_b_faces; i++) {
    c2f->idx[mesh->b_face_cells[i]+1] += 1;
    idx_size += 1;
  }

  for (cs_lnum_t i = 0; i < n_i_faces; i++) {

    int  c1_id = mesh->i_face_cells[i][0];
    int  c2_id = mesh->i_face_cells[i][1];

    if (c1_id < n_cells)
      c2f->idx[c1_id+1] += 1, idx_size += 1;
    if (c2_id < n_cells)
      c2f->idx[c2_id+1] += 1, idx_size += 1;
  }

  for (cs_lnum_t i = 0; i < n_cells; i++)
    c2f->idx[i+1] += c2f->idx[i];

  assert(c2f->idx[n_cells] == idx_size);

  BFT_MALLOC(c2f->col_id, idx_size, cs_lnum_t);
  BFT_MALLOC(c2f->sgn, idx_size, short int);

  for (cs_lnum_t f_id = 0; f_id < n_i_faces; f_id++) {

    const cs_lnum_t  c1_id = mesh->i_face_cells[f_id][0];
    const cs_lnum_t  c2_id = mesh->i_face_cells[f_id][1];

    if (c1_id < n_cells) { /* Don't want ghost cells */

      const cs_lnum_t  shift = c2f->idx[c1_id] + cell_shift[c1_id];
      c2f->col_id[shift] = f_id;
      c2f->sgn[shift] = 1;
      cell_shift[c1_id] += 1;

    }

    if (c2_id < n_cells) { /* Don't want ghost cells */

      const cs_lnum_t  shift = c2f->idx[c2_id] + cell_shift[c2_id];
      c2f->col_id[shift] = f_id;
      c2f->sgn[shift] = -1;
      cell_shift[c2_id] += 1;

    }

  } /* End of loop on internal faces */

  for (cs_lnum_t  f_id = 0; f_id < n_b_faces; f_id++) {

    const cs_lnum_t  c_id = mesh->b_face_cells[f_id];
    const cs_lnum_t  shift = c2f->idx[c_id] + cell_shift[c_id];

    c2f->col_id[shift] = n_i_faces + f_id;
    c2f->sgn[shift] = 1;
    cell_shift[c_id] += 1;

  } /* End of loop on border faces */

  /* Free memory */
  BFT_FREE(cell_shift);

  return c2f;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Build additional connectivities for accessing geometrical quantities
 *        c2e: cell --> edges connectivity
 *        c2v: cell --> vertices connectivity
 *
 * \param[in, out]  connect     pointer to the cs_cdo_connect_t struct.
 */
/*----------------------------------------------------------------------------*/

static void
_build_additional_connect(cs_cdo_connect_t  *connect)
{
  cs_connect_index_t  *c2f = cs_index_map(connect->c2f->n_rows,
                                          connect->c2f->idx,
                                          connect->c2f->col_id);
  cs_connect_index_t  *f2e = cs_index_map(connect->f2e->n_rows,
                                          connect->f2e->idx,
                                          connect->f2e->col_id);
  cs_connect_index_t  *e2v = cs_index_map(connect->e2v->n_rows,
                                          connect->e2v->idx,
                                          connect->e2v->col_id);

  /* Build new connectivity */
  connect->c2e = cs_index_compose(connect->e2v->n_rows, c2f, f2e);
  connect->c2v = cs_index_compose(connect->v2e->n_rows, connect->c2e, e2v);

  /* Sort list for each entry */
  cs_index_sort(connect->c2v);
  cs_index_sort(connect->c2e);

  /* Free mapped structures */
  cs_index_free(&c2f);
  cs_index_free(&f2e);
  cs_index_free(&e2v);

}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Compute max number of entities by cell
 *
 * \param[in]  connect     pointer to the cs_cdo_connect_t struct.
 */
/*----------------------------------------------------------------------------*/

static void
_compute_max_ent(cs_cdo_connect_t  *connect)
{
  /* Max number of faces for a cell */
  connect->n_max_fbyc = 0;
  if (connect->c2f != NULL) {
    for (int i = 0; i < connect->c2f->n_rows; i++) {
      int n_ent = connect->c2f->idx[i+1] - connect->c2f->idx[i];
      if (n_ent > connect->n_max_fbyc)
        connect->n_max_fbyc = n_ent;
    }
  }

  /* Max number of edges for a cell */
  connect->n_max_ebyc = 0;
  if (connect->c2e != NULL) {
    for (int i = 0; i < connect->c2e->n; i++) {
      int n_ent = connect->c2e->idx[i+1] - connect->c2e->idx[i];
      if (n_ent > connect->n_max_ebyc)
        connect->n_max_ebyc = n_ent;
    }
  }

  /* Max number of vertices for a cell */
  connect->n_max_vbyc = 0;
  if (connect->c2v != NULL) {
    for (int i = 0; i < connect->c2v->n; i++) {
      int n_ent = connect->c2v->idx[i+1] - connect->c2v->idx[i];
      if (n_ent > connect->n_max_vbyc)
        connect->n_max_vbyc = n_ent;
    }
  }

  /* Max number of vertices (or edges) in a face */
  connect->n_max_vbyf = 0;
  if (connect->f2e != NULL)
    connect->n_max_vbyf = connect->f2e->info.stencil_max;

}

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Allocated and initialize a cs_connect_info_t structure
 *
 * \param[in]     n_elts    Size of the maximal set of entities related to
 *                          this structure
 *
 * \return  a pointer to the new allocated structure
 */
/*----------------------------------------------------------------------------*/

static cs_connect_info_t *
_connect_info_create(cs_lnum_t     n_elts)
{
  cs_lnum_t  i;

  cs_connect_info_t  *info = NULL;

  if (n_elts < 1)
    return NULL;

  BFT_MALLOC(info, 1, cs_connect_info_t);

  info->n_elts = n_elts;
  info->n_i_elts = 0;
  info->n_b_elts = 0;

  BFT_MALLOC(info->flag, n_elts, cs_flag_t);
  for (i = 0; i < n_elts; i++)
    info->flag[i] = 0;

  return info;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Allocated and initialize a cs_cdo_connect_info_t structure
 *
 * \param[in]     info      Info structure
 *
 * \return  a pointer to the new allocated structure
 */
/*----------------------------------------------------------------------------*/

static cs_connect_info_t *
_connect_info_free(cs_connect_info_t    *info)
{
  if (info == NULL)
    return info;

  BFT_FREE(info->flag);
  BFT_FREE(info);

  return NULL;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Define a status Int/Border
 *
 * \param[in]       m        pointer to a cs_mesh_t structure
 * \param[in, out]  connect  pointer to a cs_cdo_connect_t struct.
 */
/*----------------------------------------------------------------------------*/

static void
_define_connect_info(const cs_mesh_t    *m,
                     cs_cdo_connect_t   *connect)
{
  int  i, j, v_id, e_id, f_id, c_id;

  cs_connect_info_t  *vi = NULL, *ei = NULL, *fi = NULL, *ci = NULL;

  const cs_lnum_t  n_vertices = connect->v2e->n_rows;
  const cs_lnum_t  n_edges = connect->e2f->n_rows;
  const cs_lnum_t  n_faces = connect->f2e->n_rows;
  const cs_lnum_t  n_cells = connect->c2f->n_rows;

  /* Allocate info structures */
  vi = _connect_info_create(n_vertices);
  ei = _connect_info_create(n_edges);
  fi = _connect_info_create(n_faces);
  ci = _connect_info_create(n_cells);

  /* By default all entities are set to the flag CS_CDO_CONNECT_IN (interior) */
  for (i = 0; i < n_vertices; i++)
    vi->flag[i] = CS_CDO_CONNECT_IN;
  for (i = 0; i < n_edges; i++)
    ei->flag[i] = CS_CDO_CONNECT_IN;
  for (i = 0; i < n_faces; i++)
    fi->flag[i] = CS_CDO_CONNECT_IN;
  for (i = 0; i < n_cells; i++)
    ci->flag[i] = CS_CDO_CONNECT_IN;

  /* Loop on border faces and flag all border entities
     -> cells, faces, edges and vertices */
  for (f_id = m->n_i_faces; f_id < n_faces; f_id++) {

    /* Flag related face */
    fi->flag[f_id] = CS_CDO_CONNECT_BD;
    assert(connect->f2c->idx[f_id+1]-connect->f2c->idx[f_id]==1);

    /* Flag related cell */
    c_id = connect->f2c->col_id[connect->f2c->idx[f_id]];
    ci->flag[c_id] = CS_CDO_CONNECT_BD;

    for (i = connect->f2e->idx[f_id]; i < connect->f2e->idx[f_id+1]; i++) {

      /* Flag related edges */
      e_id = connect->f2e->col_id[i];
      ei->flag[e_id] = CS_CDO_CONNECT_BD;
      for (j = connect->e2v->idx[e_id]; j < connect->e2v->idx[e_id+1]; j++) {

        /* Flag related edges */
        v_id = connect->e2v->col_id[j];
        vi->flag[v_id] = CS_CDO_CONNECT_BD;

      } /* Loop on border vertices */

    } /* Loop on border edges */

  } /* Loop on border faces */

  /* Count number of border vertices */
  for (i = 0; i < n_vertices; i++)
    if (vi->flag[i] & CS_CDO_CONNECT_BD)
      vi->n_b_elts++;
  vi->n_i_elts = vi->n_elts - vi->n_b_elts;

  /* Count number of border edges */
  for (i = 0; i < n_edges; i++)
    if (ei->flag[i] & CS_CDO_CONNECT_BD)
      ei->n_b_elts++;
  ei->n_i_elts = ei->n_elts - ei->n_b_elts;

  /* Count number of border faces */
  for (i = 0; i < n_faces; i++)
    if (fi->flag[i] & CS_CDO_CONNECT_BD)
      fi->n_b_elts++;
  fi->n_i_elts = fi->n_elts - fi->n_b_elts;

  /* Sanity check */
  assert(m->n_i_faces == fi->n_i_elts);

  /* Count number of border cells */
  for (i = 0; i < n_cells; i++)
    if (ci->flag[i] & CS_CDO_CONNECT_BD)
      ci->n_b_elts++;
  ci->n_i_elts = ci->n_elts - ci->n_b_elts;

  /* Return pointers */
  connect->v_info = vi;
  connect->e_info = ei;
  connect->f_info = fi;
  connect->c_info = ci;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Associate to each cell a type of element (fvm_element_t)
 *
 * \param[in]  c_id      cell id
 * \param[in]  connect   pointer to a cs_cdo_connect_t struct.
 *
 * \return  type of element for this cell
 */
/*----------------------------------------------------------------------------*/

static fvm_element_t
_get_cell_type(cs_lnum_t                 c_id,
               const cs_cdo_connect_t   *connect)
{
  fvm_element_t  ret_type = FVM_CELL_POLY; // Default value

  int  n_vc = connect->c2v->idx[c_id+1] - connect->c2v->idx[c_id];
  int  n_ec = connect->c2e->idx[c_id+1] - connect->c2e->idx[c_id];
  int  n_fc = connect->c2f->idx[c_id+1] - connect->c2f->idx[c_id];

  /* Tetrahedron */
  if (n_vc == 4 && n_ec == 6 && n_fc == 4)
    ret_type = FVM_CELL_TETRA;

  /* Pyramid */
  else if (n_vc == 5 && n_ec == 8 && n_fc == 5)
    ret_type = FVM_CELL_PYRAM;

  /* Prism ? */
  else if (n_vc == 6 && n_ec == 9 && n_fc == 5) { // Potentially a prism

    int  count[2] = {0, 0};

    /* Loop on cell faces */
    for (cs_lnum_t i = connect->c2f->idx[c_id]; i < connect->c2f->idx[c_id+1];
         i++) {

      cs_lnum_t  f_id = connect->c2f->col_id[i];

      if (connect->f2e->idx[f_id+1] - connect->f2e->idx[f_id] == 4) // Quad
        count[1] += 1;
      if (connect->f2e->idx[f_id+1] - connect->f2e->idx[f_id] == 3) // Tria
        count[0] += 1;

      if (count[0] == 2 && count[1] == 3)
        ret_type = FVM_CELL_PRISM;
    }

  }

  /* Hexahedron ? */
  else if (n_vc == 8 && n_ec == 12 && n_fc == 6) { // Potentially a hexahedron

    _Bool  is_hexa = true;

    /* Loop on cell faces */
    for (cs_lnum_t i = connect->c2f->idx[c_id]; i < connect->c2f->idx[c_id+1];
         i++) {

      cs_lnum_t  f_id = connect->c2f->col_id[i];

      if (connect->f2e->idx[f_id+1] - connect->f2e->idx[f_id] != 4) {
        is_hexa = false;
        break;
      }

    }

    if (is_hexa)
      ret_type = FVM_CELL_HEXA;

  }

  return ret_type;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Associate to each cell a type of element (fvm_element_t)
 *
 * \param[in, out]  connect  pointer to a cs_cdo_connect_t struct.
 */
/*----------------------------------------------------------------------------*/

static void
_define_cell_type(cs_cdo_connect_t   *connect)
{
  /* Sanity check */
  assert(connect->c2f != NULL);
  assert(connect->c2v != NULL);
  assert(connect->c2e != NULL);

  const cs_lnum_t  n_cells = connect->c_info->n_elts;

  /* Allocate and define each cell type (by default to polyhedron) */
  BFT_MALLOC(connect->cell_type, n_cells, fvm_element_t);
  for (cs_lnum_t c_id = 0; c_id < n_cells; c_id++)
    connect->cell_type[c_id] = _get_cell_type(c_id, connect);
}

/*============================================================================
 * Public function prototypes
 *============================================================================*/

/*----------------------------------------------------------------------------*/
/*!
 * \brief  String related to flag in cs_cdo_connect_info_t
 *
 * \param[in]  flag     retrieve name for this flag
 */
/*----------------------------------------------------------------------------*/

const char *
cs_cdo_connect_flagname(short int  flag)
{
  switch (flag) {

  case CS_CDO_CONNECT_BD:
    return " Border  ";
    break;
  case CS_CDO_CONNECT_IN:
    return " Interior";
    break;
  default:
    return " Undefined";
  }

}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Allocate and define by default a cs_cdo_connect_t structure
 *
 * \param[in]  m    pointer to a cs_mesh_t structure
 *
 * \return  a pointer to a cs_cdo_connect_t structure
 */
/*----------------------------------------------------------------------------*/

cs_cdo_connect_t *
cs_cdo_connect_init(const cs_mesh_t      *m)
{
  _edge_builder_t  *builder = _create_edge_builder(m);

  cs_cdo_connect_t  *connect = NULL;

  /* Build the connectivity structure */
  BFT_MALLOC(connect, 1, cs_cdo_connect_t);

  /* Build DEC matrices related to connectivity cell / face connectivity */
  connect->c2f = _build_c2f_connect(m);
  cs_sla_matrix_set_info(connect->c2f);
  connect->f2c = cs_sla_matrix_transpose(connect->c2f);
  cs_sla_matrix_set_info(connect->f2c);

  /*  Build DEC matrices related to face / edge connectivity */
  connect->f2e = _build_f2e_connect(m, builder);
  cs_sla_matrix_set_info(connect->f2e);
  connect->e2f = cs_sla_matrix_transpose(connect->f2e);
  cs_sla_matrix_set_info(connect->e2f);

  /*  Build DEC matrices related to edge / vertex connectivity */
  connect->e2v = _build_e2v_connect(builder);
  cs_sla_matrix_set_info(connect->e2v);
  connect->v2e = cs_sla_matrix_transpose(connect->e2v);
  cs_sla_matrix_set_info(connect->v2e);

  _free_edge_builder(&builder);

  /* Build additional connectivity c2e, c2v (used to access dual faces and
     dual volumes) */
  _build_additional_connect(connect);

  /* Build a flag indicated if an element belongs to the interior or border of
     the computatinoal domain. Indicate also the related number of interior and
     border entities */
  _define_connect_info(m, connect);

  /* Max number of entities (vertices, edges and faces) by cell */
  _compute_max_ent(connect);

  /* Associate to each cell a predefined type (tetra, prism, hexa...) */
  _define_cell_type(connect);

  /* Vertex range set */
  connect->v_rs = cs_range_set_create(m->vtx_interfaces,
                                      NULL,
                                      m->n_vertices,
                                      false,   // TODO: Ask Yvan
                                      0);      // g_id_base

  connect->f_rs = NULL; // TODO

  return connect;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Destroy a cs_cdo_connect_t structure
 *
 * \param[in]  connect     pointer to the cs_cdo_connect_t struct. to destroy
 *
 * \return a NULL pointer
 */
/*----------------------------------------------------------------------------*/

cs_cdo_connect_t *
cs_cdo_connect_free(cs_cdo_connect_t   *connect)
{
  if (connect == NULL)
    return connect;

  connect->v2e = cs_sla_matrix_free(connect->v2e);
  connect->e2f = cs_sla_matrix_free(connect->e2f);
  connect->e2v = cs_sla_matrix_free(connect->e2v);
  connect->f2e = cs_sla_matrix_free(connect->f2e);
  connect->f2c = cs_sla_matrix_free(connect->f2c);
  connect->c2f = cs_sla_matrix_free(connect->c2f);

  /* Specific CDO connectivity */
  cs_index_free(&(connect->c2e));
  cs_index_free(&(connect->c2v));

  connect->v_info = _connect_info_free(connect->v_info);
  connect->e_info = _connect_info_free(connect->e_info);
  connect->f_info = _connect_info_free(connect->f_info);
  connect->c_info = _connect_info_free(connect->c_info);

  BFT_FREE(connect->cell_type);

  /* Structures for parallelism */
  cs_range_set_destroy(&(connect->v_rs));
  cs_range_set_destroy(&(connect->f_rs));

  BFT_FREE(connect);

  return NULL;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Summary of connectivity information
 *
 * \param[in]  connect     pointer to cs_cdo_connect_t structure
 */
/*----------------------------------------------------------------------------*/

void
cs_cdo_connect_summary(const cs_cdo_connect_t  *connect)
{
  cs_lnum_t  n_max_entbyc[3] = {connect->n_max_fbyc,
                                connect->n_max_ebyc,
                                connect->n_max_vbyc};

  if (cs_glob_n_ranks > 1)
    cs_parall_max(3, CS_LNUM_TYPE, n_max_entbyc);

  /* Output */
  cs_log_printf(CS_LOG_DEFAULT, "\n Connectivity information:\n");
  cs_log_printf(CS_LOG_DEFAULT,
                " --dim-- max. number of faces by cell:    %4d\n",
                n_max_entbyc[0]);
  cs_log_printf(CS_LOG_DEFAULT,
                " --dim-- max. number of edges by cell:    %4d\n",
                n_max_entbyc[1]);
  cs_log_printf(CS_LOG_DEFAULT,
                " --dim-- max. number of vertices by cell: %4d\n\n",
                n_max_entbyc[2]);

  /* Information about the element types */
  cs_gnum_t  n_type_cells[FVM_N_ELEMENT_TYPES];
  for (int i = 0; i < FVM_N_ELEMENT_TYPES; i++)
    n_type_cells[i] = 0;

  for (cs_lnum_t i = 0; i < connect->c_info->n_elts; i++)
    n_type_cells[connect->cell_type[i]] += 1;

  if (cs_glob_n_ranks > 1)
    cs_parall_sum(FVM_N_ELEMENT_TYPES, CS_GNUM_TYPE, n_type_cells);

  cs_log_printf(CS_LOG_DEFAULT,
                " --dim-- number of tetrahedra: %8lu\n",
                n_type_cells[FVM_CELL_TETRA]);
  cs_log_printf(CS_LOG_DEFAULT,
                " --dim-- number of pyramids:   %8lu\n",
                n_type_cells[FVM_CELL_PYRAM]);
  cs_log_printf(CS_LOG_DEFAULT,
                " --dim-- number of prisms:     %8lu\n",
                n_type_cells[FVM_CELL_PRISM]);
  cs_log_printf(CS_LOG_DEFAULT,
                " --dim-- number of hexahedra:  %8lu\n",
                n_type_cells[FVM_CELL_HEXA]);
  cs_log_printf(CS_LOG_DEFAULT,
                " --dim-- number of polyhedra:  %8lu\n\n",
                n_type_cells[FVM_CELL_POLY]);


  /* Information about the distribution between interior and border entities */
  if (connect->v_info != NULL) {

    /* Re-use n_type_cells for another purpose */
    n_type_cells[0] = connect->v_info->n_elts;
    n_type_cells[1] = connect->v_info->n_i_elts;
    n_type_cells[2] = connect->v_info->n_b_elts;
    // Caution: BUG >> vertices may be shared (not only a sum)
    if (cs_glob_n_ranks > 1)
      cs_parall_sum(3, CS_GNUM_TYPE, n_type_cells);

    cs_log_printf(CS_LOG_DEFAULT,
                  "                    |   full    |   intern  |   border  |"
                  "\n --dim-- n_vertices | %9lu | %9lu | %9lu |",
                  n_type_cells[0], n_type_cells[1], n_type_cells[2]);
  }

  if (connect->e_info != NULL) {

    /* Re-use n_type_cells for another purpose */
    n_type_cells[0] = connect->e_info->n_elts;
    n_type_cells[1] = connect->e_info->n_i_elts;
    n_type_cells[2] = connect->e_info->n_b_elts;
    // Caution: BUG >> edges may be shared (not only a sum)
    if (cs_glob_n_ranks > 1)
      cs_parall_sum(3, CS_GNUM_TYPE, n_type_cells);

    cs_log_printf(CS_LOG_DEFAULT,
                  "\n --dim-- n_edges    | %9lu | %9lu | %9lu |",
                  n_type_cells[0], n_type_cells[1], n_type_cells[2]);

  }

  if (connect->f_info != NULL) {

    /* Re-use n_type_cells for another purpose */
    n_type_cells[0] = connect->f_info->n_elts;
    n_type_cells[1] = connect->f_info->n_i_elts;
    n_type_cells[2] = connect->f_info->n_b_elts;
    // Caution: BUG >> interior faces may be shared (not only a sum)
    if (cs_glob_n_ranks > 1)
      cs_parall_sum(3, CS_GNUM_TYPE, n_type_cells);

    cs_log_printf(CS_LOG_DEFAULT,
                  "\n --dim-- n_faces    | %9lu | %9lu | %9lu |",
                  n_type_cells[0], n_type_cells[1], n_type_cells[2]);

  }

  if (connect->c_info != NULL) {

    /* Re-use n_type_cells for another purpose */
    n_type_cells[0] = connect->c_info->n_elts;
    n_type_cells[1] = connect->c_info->n_i_elts;
    n_type_cells[2] = connect->c_info->n_b_elts;
    if (cs_glob_n_ranks > 1)
      cs_parall_sum(3, CS_GNUM_TYPE, n_type_cells);

    cs_log_printf(CS_LOG_DEFAULT,
                  "\n --dim-- n_cells    | %9lu | %9lu | %9lu |",
                  n_type_cells[0], n_type_cells[1], n_type_cells[2]);
  }

  cs_log_printf(CS_LOG_DEFAULT, "\n");

#if CS_CDO_CONNECT_DBG > 0 && defined(DEBUG) && !defined(NDEBUG)
  cs_cdo_connect_dump(connect);
#endif
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Dump a cs_cdo_connect_t structure
 *
 * \param[in]  connect     pointer to cs_cdo_connect_t structure
 */
/*----------------------------------------------------------------------------*/

void
cs_cdo_connect_dump(const cs_cdo_connect_t  *connect)
{
  int  lname = strlen("DumpConnect.dat") + 1;

  /* Define the name of the dump file */
  char *fname = NULL;
  if (cs_glob_n_ranks > 1) {
    lname += 6;
    BFT_MALLOC(fname, lname, char);
    sprintf(fname, "DumpConnect.%05d.dat", cs_glob_rank_id);
  }
  else {
    BFT_MALLOC(fname, lname, char);
    sprintf(fname, "DumpConnect.dat");
  }
  FILE  *fdump = fopen(fname, "w");

  if (connect == NULL) {
    fprintf(fdump, "Empty structure.\n");
    fclose(fdump);
    return;
  }

  fprintf(fdump, "\n Connect structure: %p\n", (const void *)connect);

  /* Dump CONNECT matrices */
  cs_sla_matrix_dump("Connect c2f mat", fdump, connect->c2f);
  cs_sla_matrix_dump("Connect f2c mat", fdump, connect->f2c);
  cs_sla_matrix_dump("Connect f2e mat", fdump, connect->f2e);
  cs_sla_matrix_dump("Connect e2f mat", fdump, connect->e2f);
  cs_sla_matrix_dump("Connect e2v mat", fdump, connect->e2v);
  cs_sla_matrix_dump("Connect v2e mat", fdump, connect->v2e);

  /* Dump specific CDO connectivity */
  cs_index_dump("Connect c2e", fdump, connect->c2e);
  cs_index_dump("Connect c2v", fdump, connect->c2v);

  fclose(fdump);
  BFT_FREE(fname);
}

/*----------------------------------------------------------------------------*/

END_C_DECLS
