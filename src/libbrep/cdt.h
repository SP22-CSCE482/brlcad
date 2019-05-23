/*                        C D T . H 
 * BRL-CAD
 *
 * Copyright (c) 2007-2019 United States Government as represented by
 * the U.S. Army Research Laboratory.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * version 2.1 as published by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this file; see the file named COPYING for more
 * information.
 */
/** @addtogroup libbrep */
/** @{ */
/** @file cdt.h
 *
 * Constrained Delaunay Triangulation of NURBS B-Rep objects.
 *
 */

#include "common.h"

#include <vector>
#include <list>
#include <map>
#include <stack>
#include <iostream>
#include <algorithm>
#include <set>
#include <utility>

#include "poly2tri/poly2tri.h"

#include "assert.h"

#include "vmath.h"

#include "bu/color.h"
#include "bu/cv.h"
#include "bu/opt.h"
#include "bu/time.h"
#include "bn/plot3.h"
#include "bn/tol.h"
#include "bn/vlist.h"
#include "bg/polygon.h"
#include "bg/trimesh.h"
#include "brep/defines.h"
#include "brep/cdt.h"
#include "brep/pullback.h"
#include "brep/util.h"


/***************************************************/
typedef std::pair<ON_3dPoint *, ON_3dPoint *> Edge;
typedef std::map< Edge, std::set<p2t::Triangle*> > EdgeToTri;

#define BREP_CDT_FAILED -3
#define BREP_CDT_NON_SOLID -2
#define BREP_CDT_UNTESSELLATED -1
#define BREP_CDT_SOLID 0

/* Note - these tolerance values are based on the default
 * values from the GED 'tol' command */
#define BREP_CDT_DEFAULT_TOL_ABS 0.0
#define BREP_CDT_DEFAULT_TOL_REL 0.01
#define BREP_CDT_DEFAULT_TOL_NORM 0.0
#define BREP_CDT_DEFAULT_TOL_DIST BN_TOL_DIST

/* this is a debugging structure - it holds origin information for
 * a point added to the CDT state */
struct cdt_audit_info {
    int face_index;
    int vert_index;
    int trim_index;
    int edge_index;
    ON_2dPoint surf_uv;
};

struct ON_Brep_CDT_State {

    int status;
    ON_Brep *brep;

    /* Tolerances */
    fastf_t abs;
    fastf_t rel;
    fastf_t norm;
    fastf_t dist;

    /* 3D data */
    std::vector<ON_3dPoint *> *w3dpnts;
    std::map<int, ON_3dPoint *> *vert_pnts;
    std::vector<ON_3dPoint *> *w3dnorms;
    std::map<int, ON_3dPoint *> *vert_norms;

    /* singular trim info */
    std::map<int, std::map<int,ON_3dPoint *>> *strim_pnts;
    std::map<int, std::map<int,ON_3dPoint *>> *strim_norms;

    /* Poly2Tri data */
    p2t::CDT **p2t_faces;
    std::vector<p2t::Triangle *> **p2t_extra_faces;
    std::map<ON_2dPoint *, ON_3dPoint *> **on2_to_on3_maps;
    std::map<p2t::Point *, ON_3dPoint *> **tri_to_on3_maps;
    std::map<p2t::Point *, ON_3dPoint *> **tri_to_on3_norm_maps;
    std::map<ON_3dPoint *, std::set<p2t::Point *>> **on3_to_tri_maps;

    /* Audit data */
    std::map<ON_3dPoint *, struct cdt_audit_info *> pnt_audit_info;

    /* BoT -> ON mappings */
    std::map<ON_3dPoint *, std::set<BrepTrimPoint *>> on_brep_edge_pnts;
    std::map<int, ON_3dPoint *> *vert_to_on;
    std::set<ON_3dPoint *> *edge_pnts;
    ON_SimpleArray<BrepTrimPoint> **brep_face_loop_points;
    std::set<p2t::Point *> **face_degen_pnts;
};

struct brep_cdt_tol {
    fastf_t min_dist;
    fastf_t max_dist;
    fastf_t within_dist;
    fastf_t cos_within_ang;
};
#define BREP_CDT_TOL_ZERO {0.0, 0.0, 0.0, 0.0}

struct cdt_surf_info {
    const ON_Surface *s;
    const ON_BrepFace *f;
    std::map<int, std::map<int,ON_3dPoint *>> *strim_pnts;
    std::map<int, std::map<int,ON_3dPoint *>> *strim_norms;
    double u1, u2, v1, v2;
    fastf_t ulen;
    fastf_t u_lower_3dlen;
    fastf_t u_mid_3dlen;
    fastf_t u_upper_3dlen;
    fastf_t vlen;
    fastf_t v_lower_3dlen;
    fastf_t v_mid_3dlen;
    fastf_t v_upper_3dlen;
    fastf_t min_edge;
    fastf_t max_edge;
};


void
PerformClosedSurfaceChecks(
	struct ON_Brep_CDT_State *s_cdt,
	const ON_Surface *s,
	const ON_BrepFace &face,
	ON_SimpleArray<BrepTrimPoint> *brep_loop_points,
	double same_point_tolerance);

std::map<double, BrepTrimPoint *> *
getEdgePoints(
	struct ON_Brep_CDT_State *s_cdt,
	ON_BrepEdge *edge,
	ON_BrepTrim &trim,
	fastf_t max_dist,
	std::map<int, ON_3dPoint *> *vert_pnts,
	std::map<int, ON_3dPoint *> *vert_norms
	);

void
getSurfacePoints(struct ON_Brep_CDT_State *s_cdt,
	         const ON_BrepFace &face,
		 ON_2dPointArray &on_surf_points,
		 fastf_t min_edge, fastf_t max_edge);

void
plot_polyline(std::vector<p2t::Point *> *pnts, const char *filename);
void
plot_tri(p2t::Triangle *t, const char *filename);

struct cdt_audit_info *
cdt_ainfo(int fid, int vid, int tid, int eid, fastf_t x2d, fastf_t y2d);

void
add_tri_edges(EdgeToTri *e2f, p2t::Triangle *t,
    std::map<p2t::Point *, ON_3dPoint *> *pointmap);

void
CDT_Add3DPnt(struct ON_Brep_CDT_State *s, ON_3dPoint *p, int fid, int vid, int tid, int eid, fastf_t x2d, fastf_t y2d);

void
CDT_Tol_Set(struct brep_cdt_tol *cdt, double dist, fastf_t md, double t_abs, double t_rel, double t_norm, double t_dist);


/** @} */

// Local Variables:
// mode: C++
// tab-width: 8
// c-basic-offset: 4
// indent-tabs-mode: t
// c-file-style: "stroustrup"
// End:
// ex: shiftwidth=4 tabstop=8
