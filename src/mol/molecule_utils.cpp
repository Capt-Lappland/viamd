#include "molecule_utils.h"
#include <core/common.h>
#include <core/hash.h>
#include <core/log.h>
#include <mol/trajectory_utils.h>
#include <mol/spatial_hash.h>
#include <gfx/gl_utils.h>
#include <gfx/immediate_draw_utils.h>

#include <ctype.h>

void transform_positions(Array<vec3> positions, const mat4& transformation) {
    for (auto& p : positions) {
        p = vec3(transformation * vec4(p, 1));
    }
}

void compute_bounding_box(vec3* min_box, vec3* max_box, Array<const vec3> positions, Array<const float> radii) {
    ASSERT(min_box);
    ASSERT(max_box);
    if (radii.count > 0) {
        ASSERT(radii.count == positions.count);
    }

    if (positions.count == 0) {
        *min_box = *max_box = vec3(0);
    }

    *min_box = *max_box = positions.data[0];
    for (int64 i = 0; i < positions.count; i++) {
        const vec3& p = positions.data[i];
        const float r = radii.count > 0 ? radii.data[i] : 0.f;
        *min_box = math::min(*min_box, p - r);
        *max_box = math::max(*max_box, p + r);
    }
}

vec3 compute_com(Array<const vec3> positions, Array<const float> masses) {
    if (positions.count == 0) return {0, 0, 0};
    if (positions.count == 1) return positions[0];

    vec3 com{0};
    if (masses.count == 0) {
        for (const auto& p : positions) {
            com += p;
        }
        com = com / (float)positions.count;
    } else {
        ASSERT(masses.count == positions.count);
        vec3 pos_mass_sum{0};
        float mass_sum{0};
        for (int32 i = 0; i < positions.count; i++) {
            pos_mass_sum += positions[i] * masses[i];
            mass_sum += masses[i];
        }
        com = pos_mass_sum / mass_sum;
    }

    return com;
}

inline bool periodic_jump(const vec3& p_prev, const vec3& p_next, const vec3& half_box) {
    const vec3 abs_delta = math::abs(p_next - p_prev);
    if (abs_delta.x > half_box.x) return true;
    if (abs_delta.y > half_box.y) return true;
    if (abs_delta.z > half_box.z) return true;
    return false;
}

void linear_interpolation(Array<vec3> positions, Array<const vec3> prev_pos, Array<const vec3> next_pos, float t) {
    ASSERT(prev_pos.count == positions.count);
    ASSERT(next_pos.count == positions.count);

    for (int i = 0; i < positions.count; i++) {
        positions[i] = math::mix(prev_pos[i], next_pos[i], t);
    }
}

inline __m128 mm_abs(const __m128 x) { return _mm_and_ps(x, _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF))); }

inline __m128 mm_sign(const __m128 x) {
    __m128 const zro0 = _mm_setzero_ps();
    __m128 const cmp0 = _mm_cmplt_ps(x, zro0);
    __m128 const cmp1 = _mm_cmpgt_ps(x, zro0);
    __m128 const and0 = _mm_and_ps(cmp0, _mm_set1_ps(-1.0f));
    __m128 const and1 = _mm_and_ps(cmp1, _mm_set1_ps(1.0f));
    __m128 const or0 = _mm_or_ps(and0, and1);
    return or0;
}

inline glm_vec4 glm_step(const glm_vec4 edge, const glm_vec4 x) {
    const glm_vec4 cmp = _mm_cmpge_ps(x, edge);
    const glm_vec4 res = _mm_and_ps(cmp, _mm_set1_ps(1.f));
    return res;
}

inline glm_vec4 de_periodize(const glm_vec4 p0, const glm_vec4 p1, const glm_vec4 full_ext, const glm_vec4 half_ext) {
    const glm_vec4 delta = glm_vec4_sub(p1, p0);
    const glm_vec4 signed_mask = glm_vec4_mul(glm_vec4_sign(delta), glm_step(half_ext, glm_vec4_abs(delta)));
    return glm_vec4_sub(p1, glm_vec4_mul(full_ext, signed_mask));
}

// @TODO: Fix this, is it possible in theory to get a good interpolation between frames with periodicity without modifying source data?
// @PERFORMANCE: VECTORIZE THIS
void linear_interpolation_periodic(Array<vec3> positions, Array<const vec3> prev_pos, Array<const vec3> next_pos, float t, mat3 sim_box) {
    ASSERT(prev_pos.count == positions.count);
    ASSERT(next_pos.count == positions.count);

    const glm_vec4 full_box_ext = _mm_set_ps(0.f, sim_box[2][2], sim_box[1][1], sim_box[0][0]);
    const glm_vec4 half_box_ext = glm_vec4_mul(full_box_ext, _mm_set_ps1(0.5f));
    const glm_vec4 t_vec = _mm_set_ps1(t);

    for (int i = 0; i < positions.count; i++) {
        glm_vec4 next = _mm_set_ps(0, next_pos[i].z, next_pos[i].y, next_pos[i].x);
        glm_vec4 prev = _mm_set_ps(0, prev_pos[i].z, prev_pos[i].y, prev_pos[i].x);

        next = de_periodize(prev, next, full_box_ext, half_box_ext);
        const glm_vec4 res = glm_vec4_mix(prev, next, t_vec);

        positions[i] = *reinterpret_cast<const vec3*>(&res);

        /*
__m128 delta = _mm_sub_ps(next, prev);
__m128 abs_delta = mm_abs(delta);
__m128 sign_delta = mm_sign(delta);
* / glm_vec4_sign

/*
vec4 next = vec4(next_pos[i], 0);
vec4 prev = vec4(prev_pos[i], 0);

const vec4 delta = next - prev;
const vec4 sign_delta = math::sign(delta);
const vec4 abs_delta = math::abs(delta);
const vec4 signed_mask = sign_delta * math::step(half_box_ext, abs_delta);
next = next - full_box_ext * signed_mask;
*/

        // if (abs_delta.x > half_box_ext.x) next.x = next.x - sign_delta.x * full_box_ext.x;
        // if (abs_delta.y > half_box_ext.y) next.y = next.y - sign_delta.y * full_box_ext.y;
        // if (abs_delta.z > half_box_ext.z) next.z = next.z - sign_delta.z * full_box_ext.z;

        // vec3 signed_mask = math::sign(delta) * math::step(half_box_ext, math::abs(delta));
        // next = next - full_box_ext * signed_mask;

        // Make sure we do not violate periodic bounds
        // vec3 periodic_pos = math::fract(vec3(1, 1, 1) + math::mix(prev, next, t) * inv_full_box_ext);
        // positions[i] = periodic_pos * full_box_ext;
        // positions[i] = math::mix(prev, next, t);
    }
}

void spline_interpolation_periodic(Array<vec3> positions, Array<const vec3> pos0, Array<const vec3> pos1, Array<const vec3> pos2,
                                   Array<const vec3> pos3, float t, mat3 sim_box) {
    ASSERT(pos0.count == positions.count);
    ASSERT(pos1.count == positions.count);
    ASSERT(pos2.count == positions.count);
    ASSERT(pos3.count == positions.count);

/*
    const vec3 full_box_ext = sim_box * vec3(1);
    // const vec3 inv_full_box_ext = 1.f / full_box_ext;
    const vec3 half_box_ext = full_box_ext * 0.5f;
*/
    const glm_vec4 full_box_ext = _mm_set_ps(0.f, sim_box[2][2], sim_box[1][1], sim_box[0][0]);
    const glm_vec4 half_box_ext = glm_vec4_mul(full_box_ext, _mm_set_ps1(0.5f));

    for (int i = 0; i < positions.count; i++) {
        glm_vec4 p0 = _mm_set_ps(0, pos0[i].z, pos0[i].y, pos0[i].x);
        glm_vec4 p1 = _mm_set_ps(0, pos1[i].z, pos1[i].y, pos1[i].x);
        glm_vec4 p2 = _mm_set_ps(0, pos2[i].z, pos2[i].y, pos2[i].x);
        glm_vec4 p3 = _mm_set_ps(0, pos3[i].z, pos3[i].y, pos3[i].x);

        p0 = de_periodize(p1, p0, full_box_ext, half_box_ext);
        p2 = de_periodize(p1, p2, full_box_ext, half_box_ext);
        p3 = de_periodize(p1, p3, full_box_ext, half_box_ext);

        const glm_vec4 res = math::spline(p0, p1, p2, p3, t);
        positions[i] = *reinterpret_cast<const vec3*>(&res);

        /*
        vec3 p0 = pos0[i];
        vec3 p1 = pos1[i];
        vec3 p2 = pos2[i];
        vec3 p3 = pos3[i];

        // p1 is our reference which we want to 'de-periodize' the other positions to in order to interpolate correctly
        vec3 d0 = p0 - p1;
        vec3 s0 = math::sign(d0) * math::step(half_box_ext, math::abs(d0));
        p0 = p0 - full_box_ext * s0;

        vec3 d2 = p2 - p1;
        vec3 s2 = math::sign(d2) * math::step(half_box_ext, math::abs(d2));
        p2 = p2 - full_box_ext * s2;

        vec3 d3 = p3 - p1;
        vec3 s3 = math::sign(d3) * math::step(half_box_ext, math::abs(d3));
        p3 = p3 - full_box_ext * s3;

        // Make sure we do not violate periodic bounds
        // vec3 periodic_pos = math::fract(vec3(1, 1, 1) + math::spline(p0, p1, p2, p3, t) * inv_full_box_ext);
        // positions[i] = periodic_pos * full_box_ext;
        positions[i] = math::spline(p0, p1, p2, p3, t);
        */
    }
}

void spline_interpolation(Array<vec3> positions, Array<const vec3> pos0, Array<const vec3> pos1, Array<const vec3> pos2, Array<const vec3> pos3,
                          float t) {
    ASSERT(pos0.count == positions.count);
    ASSERT(pos1.count == positions.count);
    ASSERT(pos2.count == positions.count);
    ASSERT(pos3.count == positions.count);

    for (int i = 0; i < positions.count; i++) {
        vec3 p0 = pos0[i];
        vec3 p1 = pos1[i];
        vec3 p2 = pos2[i];
        vec3 p3 = pos3[i];

        positions[i] = math::spline(p0, p1, p2, p3, t);
    }
}

inline bool covelent_bond_heuristic(const vec3& pos_a, Element elem_a, const vec3& pos_b, Element elem_b) {
    auto d = element::covalent_radius(elem_a) + element::covalent_radius(elem_b);
    auto d1 = d + 0.3f;
    auto d2 = d - 0.5f;
    auto v = pos_a - pos_b;
    auto dist2 = math::dot(v, v);
    return dist2 < (d1 * d1) && dist2 > (d2 * d2);
}

// Computes covalent bonds between a set of atoms with given positions and elements.
// The approach is inspired by the technique used in NGL (https://github.com/arose/ngl)
DynamicArray<Bond> compute_covalent_bonds(Array<const vec3> atom_pos, Array<const Element> atom_elem, Array<const ResIdx> atom_res_idx) {
    ASSERT(atom_pos.count == atom_elem.count);
    if (atom_res_idx.count > 0) {
        ASSERT(atom_pos.count == atom_res_idx.count);
    }

    constexpr float max_covelent_bond_length = 3.5f;
    spatialhash::Frame frame = spatialhash::compute_frame(atom_pos, vec3(max_covelent_bond_length));
    DynamicArray<Bond> bonds;

    if (atom_res_idx.count > 0) {
        // @NOTE: If we have residues the assumtion is that a bond is either within a single residue or between concecutive residues.
        for (int atom_i = 0; atom_i < atom_pos.count; atom_i++) {
            spatialhash::for_each_within(frame, atom_pos[atom_i], max_covelent_bond_length,
                                         [&bonds, &atom_pos, &atom_elem, &atom_res_idx, atom_i](int atom_j, const vec3& atom_j_pos) {
                                             (void)atom_j_pos;
                                             if (atom_i < atom_j && (math::abs(atom_res_idx[atom_i] - atom_res_idx[atom_j]) < 2) &&
                                                 covelent_bond_heuristic(atom_pos[atom_i], atom_elem[atom_i], atom_pos[atom_j], atom_elem[atom_j])) {
                                                 bonds.push_back({atom_i, atom_j});
                                             }
                                         });
        }
    } else {
        // @NOTE: Since we do not have any hierarchical information given, try all atoms against all other atoms.
        for (int atom_i = 0; atom_i < atom_pos.count; atom_i++) {
            spatialhash::for_each_within(
                frame, atom_pos[atom_i], max_covelent_bond_length, [&bonds, &atom_pos, &atom_elem, atom_i](int atom_j, const vec3& atom_j_pos) {
                    (void)atom_j_pos;
                    if (atom_i < atom_j && covelent_bond_heuristic(atom_pos[atom_i], atom_elem[atom_i], atom_pos[atom_j], atom_elem[atom_j])) {
                        bonds.push_back({atom_i, atom_j});
                    }
                });
        }
    }

    return bonds;
}

// @NOTE this method is sub-optimal and can surely be improved...
// Residues should have no more than 2 potential connections to other residues.
DynamicArray<Chain> compute_chains(Array<const Residue> residues, Array<const Bond> bonds, Array<const ResIdx> atom_residue_indices) {
    DynamicArray<Bond> residue_bonds;

    if (atom_residue_indices) {
        for (const auto& bond : bonds) {
            if (atom_residue_indices[bond.idx_a] != atom_residue_indices[bond.idx_b]) {
                residue_bonds.push_back({atom_residue_indices[bond.idx_a], atom_residue_indices[bond.idx_b]});
            }
        }
    } else {
        ASSERT(false, "Not implemented Yeti");
    }

    if (residue_bonds.count == 0) {
        // No residue bonds, No chains.
        return {};
    }

    DynamicArray<int> residue_chains(residues.count, -1);

    if (residue_bonds.count > 0) {
        int curr_chain_idx = 0;
        int res_bond_idx = 0;
        for (int i = 0; i < residues.count; i++) {
            if (residue_chains[i] == -1) residue_chains[i] = curr_chain_idx++;
            for (; res_bond_idx < residue_bonds.count; res_bond_idx++) {
                const auto& res_bond = residue_bonds[res_bond_idx];
                if (i == res_bond.idx_a) {
                    residue_chains[res_bond.idx_b] = residue_chains[res_bond.idx_a];
                } else if (res_bond.idx_a > i)
                    break;
            }
        }
    }

    DynamicArray<Chain> chains;
    int curr_chain_idx = -1;
    for (int i = 0; i < residue_chains.count; i++) {
        if (residue_chains[i] != curr_chain_idx) {
            curr_chain_idx = residue_chains[i];
            Label lbl;
            snprintf(lbl.beg(), Label::MAX_LENGTH, "C%i", curr_chain_idx);
            chains.push_back({lbl, (ResIdx)i, (ResIdx)i});
        }
        chains.back().end_res_idx++;
    }

    return chains;
}

template <int64 N>
bool match(const Label& lbl, const char (&cstr)[N]) {
    for (int64 i = 0; i < N; i++) {
        if (tolower(lbl[i]) != tolower(cstr[i])) return false;
    }
    return true;
}

DynamicArray<BackboneSegment> compute_backbone_segments(Array<const Residue> residues, Array<const Label> atom_labels) {
    DynamicArray<BackboneSegment> segments;
    int64 invalid_segments = 0;
    for (auto& res : residues) {
        auto ca_idx = -1;
        auto n_idx = -1;
        auto c_idx = -1;
        auto o_idx = -1;
        if (is_amino_acid(res)) {
            // find atoms
            for (int32 i = res.beg_atom_idx; i < res.end_atom_idx; i++) {
                const auto& lbl = atom_labels[i];
                if (ca_idx == -1 && match(lbl, "CA")) ca_idx = i;
                if (n_idx == -1 && match(lbl, "N")) n_idx = i;
                if (c_idx == -1 && match(lbl, "C")) c_idx = i;
                if (o_idx == -1 && match(lbl, "O")) o_idx = i;
            }

            // Could not match "O"
            if (o_idx == -1) {
                // Pick first atom containing O after C atom
                for (int32 i = c_idx; i < res.end_atom_idx; i++) {
                    const auto& lbl = atom_labels[i];
                    if (lbl[0] == 'o' || lbl[0] == 'O') o_idx = i;
                }
            }

            if (ca_idx == -1 || n_idx == -1 || c_idx == -1 || o_idx == -1) {
                LOG_ERROR("Could not identify all backbone indices for residue %s.\n", res.name.beg());
                invalid_segments++;
            }
            segments.push_back({ca_idx, n_idx, c_idx, o_idx});
        } else {
            segments.push_back({-1, -1, -1, -1});
            invalid_segments++;
        }
    }

    if (invalid_segments == segments.count) return {};

    return segments;
}

DynamicArray<SplineSegment> compute_spline(Array<const vec3> atom_pos, Array<const uint32> colors, Array<const BackboneSegment> backbone,
                                           int num_subdivisions, float tension) {
    if (backbone.count < 4) return {};

    DynamicArray<vec3> p_tmp;
    DynamicArray<vec3> o_tmp;
    DynamicArray<vec3> c_tmp;
    DynamicArray<int> ca_idx;

    auto d_p0 = atom_pos[backbone[1].ca_idx] - atom_pos[backbone[0].ca_idx];
    p_tmp.push_back(atom_pos[backbone[0].ca_idx] - d_p0);

    auto d_o0 = atom_pos[backbone[1].o_idx] - atom_pos[backbone[0].o_idx];
    o_tmp.push_back(atom_pos[backbone[0].o_idx] - d_o0);

    auto d_c0 = atom_pos[backbone[1].c_idx] - atom_pos[backbone[0].c_idx];
    c_tmp.push_back(atom_pos[backbone[0].c_idx] - d_c0);

    ca_idx.push_back(backbone[0].ca_idx);

    const int size = (int)(backbone.count);
    for (auto i = 0; i < size; i++) {
        p_tmp.push_back(atom_pos[backbone[i].ca_idx]);
        o_tmp.push_back(atom_pos[backbone[i].o_idx]);
        c_tmp.push_back(atom_pos[backbone[i].c_idx]);
        ca_idx.push_back(backbone[i].ca_idx);
    }

    auto d_pn = atom_pos[backbone[size - 1].ca_idx] - atom_pos[backbone[size - 2].ca_idx];
    p_tmp.push_back(atom_pos[backbone[size - 1].ca_idx] + d_pn);
    p_tmp.push_back(p_tmp.back() + d_pn);

    auto d_on = atom_pos[backbone[size - 1].o_idx] - atom_pos[backbone[size - 2].o_idx];
    o_tmp.push_back(atom_pos[backbone[size - 1].o_idx] + d_on);
    o_tmp.push_back(o_tmp.back() + d_on);

    auto d_cn = atom_pos[backbone[size - 1].c_idx] - atom_pos[backbone[size - 2].c_idx];
    c_tmp.push_back(atom_pos[backbone[size - 1].c_idx] + d_cn);
    c_tmp.push_back(c_tmp.back() + d_cn);

    ca_idx.push_back(backbone[size - 1].ca_idx);
    ca_idx.push_back(backbone[size - 1].ca_idx);

    // NEEDED?
    for (int64 i = 1; i < o_tmp.size(); i++) {
        vec3 v0 = o_tmp[i - 1] - c_tmp[i - 1];
        vec3 v1 = o_tmp[i] - c_tmp[i];

        if (glm::dot(v0, v1) < 0) {
            o_tmp[i] = c_tmp[i] - v1;
        }
    }

    DynamicArray<SplineSegment> segments;

    for (int64 i = 1; i < p_tmp.size() - 2; i++) {
        auto p0 = p_tmp[i - 1];
        auto p1 = p_tmp[i];
        auto p2 = p_tmp[i + 1];
        auto p3 = p_tmp[i + 2];

        auto o0 = o_tmp[i - 1];
        auto o1 = o_tmp[i];
        auto o2 = o_tmp[i + 1];
        auto o3 = o_tmp[i + 2];

        auto c0 = c_tmp[i - 1];
        auto c1 = c_tmp[i];
        auto c2 = c_tmp[i + 1];
        auto c3 = c_tmp[i + 2];

        uint32 idx = ca_idx[i];
        uint32 color = colors[idx];

        auto count = (i < (p_tmp.size() - 3)) ? num_subdivisions : num_subdivisions + 1;
        for (int n = 0; n < count; n++) {
            auto t = n / (float)(num_subdivisions);

            vec3 p = math::spline(p0, p1, p2, p3, t, tension);
            vec3 o = math::spline(o0, o1, o2, o3, t, tension);
            vec3 c = math::spline(c0, c1, c2, c3, t, tension);

            vec3 v_dir = math::normalize(o - c);

            const float eps = 0.0001f;
            float d0 = math::max(0.f, t - eps);
            float d1 = math::min(t + eps, 1.f);

            vec3 tangent = math::normalize(math::spline(p0, p1, p2, p3, d1, tension) - math::spline(p0, p1, p2, p3, d0, tension));
            vec3 normal = math::normalize(math::cross(v_dir, tangent));
            vec3 binormal = math::normalize(math::cross(tangent, normal));
            // vec3 normal = v_dir;
            // vec3 binormal = math::normalize(math::cross(tangent, normal));

            segments.push_back({p, tangent, normal, binormal, idx, color});
        }
    }

    return segments;
}

DynamicArray<BackboneAngles> compute_backbone_angles(Array<const vec3> pos, Array<const BackboneSegment> backbone) {
    if (backbone.count == 0) return {};
    DynamicArray<BackboneAngles> angles(backbone.count);
    compute_backbone_angles(angles, pos, backbone);
    return angles;
}

void compute_backbone_angles(Array<BackboneAngles> dst, Array<const vec3> pos, Array<const BackboneSegment> backbone_segments) {
    ASSERT(dst.count >= backbone_segments.count);
    float omega, phi, psi;

    omega = 0;
    phi = 0;
    psi = math::dihedral_angle(pos[backbone_segments[0].n_idx], pos[backbone_segments[0].ca_idx], pos[backbone_segments[0].c_idx],
                               pos[backbone_segments[1].n_idx]);
    dst[0] = {omega, phi, psi};

    for (int64 i = 1; i < backbone_segments.count - 1; i++) {
        omega = math::dihedral_angle(pos[backbone_segments[i - 1].ca_idx], pos[backbone_segments[i - 1].c_idx], pos[backbone_segments[i].n_idx],
                                     pos[backbone_segments[i].ca_idx]);
        phi = math::dihedral_angle(pos[backbone_segments[i - 1].c_idx], pos[backbone_segments[i].n_idx], pos[backbone_segments[i].ca_idx],
                                   pos[backbone_segments[i].c_idx]);
        psi = math::dihedral_angle(pos[backbone_segments[i].n_idx], pos[backbone_segments[i].ca_idx], pos[backbone_segments[i].c_idx],
                                   pos[backbone_segments[i + 1].n_idx]);
        dst[i] = {omega, phi, psi};
    }

    auto N = backbone_segments.count - 1;
    omega = math::dihedral_angle(pos[backbone_segments[N - 1].ca_idx], pos[backbone_segments[N - 1].c_idx], pos[backbone_segments[N].n_idx],
                                 pos[backbone_segments[N].ca_idx]);
    phi = math::dihedral_angle(pos[backbone_segments[N - 1].c_idx], pos[backbone_segments[N].n_idx], pos[backbone_segments[N].ca_idx],
                               pos[backbone_segments[N].c_idx]);
    psi = 0;
    dst[N] = {omega, phi, psi};
}

void init_backbone_angles_trajectory(BackboneAnglesTrajectory* data, const MoleculeDynamic& dynamic) {
    ASSERT(data);
    if (!dynamic.molecule || !dynamic.trajectory) return;

    if (data->angle_data) {
        FREE(data->angle_data.data);
    }

    int32 alloc_count = (int32)dynamic.molecule.backbone_segments.count * (int32)dynamic.trajectory.frame_buffer.count;
    data->num_segments = (int32)dynamic.molecule.backbone_segments.count;
    data->num_frames = 0;
    data->angle_data = {(BackboneAngles*)CALLOC(alloc_count, sizeof(BackboneAngles)), alloc_count};
}

void free_backbone_angles_trajectory(BackboneAnglesTrajectory* data) {
    ASSERT(data);
    if (data->angle_data) {
        FREE(data->angle_data.data);
        *data = {};
    }
}

void compute_backbone_angles_trajectory(BackboneAnglesTrajectory* data, const MoleculeDynamic& dynamic) {
    ASSERT(dynamic.trajectory && dynamic.molecule);
    if (dynamic.trajectory.num_frames == 0 || dynamic.molecule.backbone_segments.count == 0) return;

    //@NOTE: Trajectory may be loading while this is taking place, therefore read num_frames once and stick to that
    int32 traj_num_frames = dynamic.trajectory.num_frames;

    // @NOTE: If we are up to date, no need to compute anything
    if (traj_num_frames == data->num_frames) {
        return;
    }

    // @TODO: parallelize?
    // @NOTE: Only compute data for indices which are new
    for (int32 f_idx = data->num_frames; f_idx < traj_num_frames; f_idx++) {
        Array<const vec3> frame_pos = get_trajectory_positions(dynamic.trajectory, f_idx);
        Array<BackboneAngles> frame_angles = get_backbone_angles(*data, f_idx);
        for (const Chain& c : dynamic.molecule.chains) {
            auto bb_segments = get_backbone(dynamic.molecule, c);
            auto bb_angles = frame_angles.sub_array(c.beg_res_idx, c.end_res_idx - c.beg_res_idx);
            compute_backbone_angles(bb_angles, frame_pos, bb_segments);
        }
    }
}

DynamicArray<float> compute_atom_radii(Array<const Element> elements) {
    DynamicArray<float> radii(elements.count, 0);
    compute_atom_radii(radii, elements);
    return radii;
}

void compute_atom_radii(Array<float> radii_dst, Array<const Element> elements) {
    ASSERT(radii_dst.count <= elements.count);
    for (int64 i = 0; i < radii_dst.count; i++) {
        radii_dst[i] = element::vdw_radius(elements[i]);
    }
}

DynamicArray<uint32> compute_atom_colors(const MoleculeStructure& mol, ColorMapping mapping, uint32 static_color) {
    DynamicArray<uint32> colors(mol.atom_elements.count, 0xFFFFFFFF);
    compute_atom_colors(colors, mol, mapping, static_color);
    return colors;
}

static inline vec3 compute_color(uint32 hash) {
    constexpr float CHROMA = 0.45f;
    constexpr float LUMINANCE = 0.90f;
    constexpr int32 MOD = 21;
    constexpr float SCL = 1.f / (float)MOD;

    return math::hcl_to_rgb(vec3((hash % MOD) * SCL, CHROMA, LUMINANCE));
}

void compute_atom_colors(Array<uint32> color_dst, const MoleculeStructure& mol, ColorMapping mapping, uint32 static_color) {
    // @TODO: Implement more mappings

    // CPK
    switch (mapping) {
        case ColorMapping::STATIC_COLOR:
            for (int64 i = 0; i < color_dst.count; i++) {
                color_dst[i] = static_color;
            }
            break;
        case ColorMapping::CPK:
            for (int64 i = 0; i < color_dst.count; i++) {
                color_dst[i] = element::color(mol.atom_elements[i]);
            }
            break;
        case ColorMapping::RES_ID:
            // Color based on residues, not unique by any means.
            // Perhaps use predefined colors if residues are Amino acids
            for (int64 i = 0; i < color_dst.count; i++) {
                if (i < mol.atom_residue_indices.count) {
                    const auto& res = mol.residues[mol.atom_residue_indices[i]];
                    vec3 c = compute_color(hash::crc32(res.name.operator CString()));
                    unsigned char color[4];
                    color[0] = (unsigned char)(c.x * 255);
                    color[1] = (unsigned char)(c.y * 255);
                    color[2] = (unsigned char)(c.z * 255);
                    color[3] = (unsigned char)255;
                    color_dst[i] = *(uint32*)(color);
                }
            }
            break;
        case ColorMapping::RES_INDEX:
            for (int64 i = 0; i < color_dst.count; i++) {
                if (i < mol.atom_residue_indices.count) {
                    vec3 c = compute_color(mol.atom_residue_indices[i]);
                    unsigned char color[4];
                    color[0] = (unsigned char)(c.x * 255);
                    color[1] = (unsigned char)(c.y * 255);
                    color[2] = (unsigned char)(c.z * 255);
                    color[3] = (unsigned char)(255);
                    color_dst[i] = *(uint32*)(color);
                }
            }
            break;
        case ColorMapping::CHAIN_ID:
            for (int64 i = 0; i < color_dst.count; i++) {
                if (i < mol.atom_residue_indices.count) {
                    const auto& res = mol.residues[mol.atom_residue_indices[i]];
                    if (res.chain_idx < mol.chains.count) {
                        vec3 c = compute_color(hash::crc32(res.name.operator CString()));
                        unsigned char color[4];
                        color[0] = (unsigned char)(c.x * 255);
                        color[1] = (unsigned char)(c.y * 255);
                        color[2] = (unsigned char)(c.z * 255);
                        color[3] = (unsigned char)(255);
                        color_dst[i] = *(uint32*)(color);
                    }
                }
            }
        case ColorMapping::CHAIN_INDEX:
            for (int64 i = 0; i < color_dst.count; i++) {
                if (i < mol.atom_residue_indices.count) {
                    const auto& res = mol.residues[mol.atom_residue_indices[i]];
                    if (res.chain_idx < mol.chains.count) {
                        vec3 c = compute_color(res.chain_idx);
                        unsigned char color[4];
                        color[0] = (unsigned char)(c.x * 255);
                        color[1] = (unsigned char)(c.y * 255);
                        color[2] = (unsigned char)(c.z * 255);
                        color[3] = (unsigned char)(255);
                        color_dst[i] = *(uint32*)(color);
                    }
                }
            }
        default:
            break;
    }
}

mat4 compute_linear_transform(Array<const vec3> pos_frame_a, Array<const vec3> pos_frame_b) {
    ASSERT(pos_frame_a.count == pos_frame_b.count);

    const vec3 com_a = compute_com(pos_frame_a);
    DynamicArray<vec3> q(pos_frame_a.count);
    for (int32 i = 0; i < q.count; i++) {
        q[i] = pos_frame_a[i] - com_a;
    }

    const vec3 com_b = compute_com(pos_frame_b);
    DynamicArray<vec3> p(pos_frame_b.count);
    for (int32 i = 0; i < p.count; i++) {
        p[i] = pos_frame_b[i] - com_b;
    }

    mat3 Apq{0};
    for (int32 i = 0; i < p.count; i++) {
        Apq[0][0] += p[i].x * q[i].x;
        Apq[0][1] += p[i].y * q[i].x;
        Apq[0][2] += p[i].z * q[i].x;
        Apq[1][0] += p[i].x * q[i].y;
        Apq[1][1] += p[i].y * q[i].y;
        Apq[1][2] += p[i].z * q[i].y;
        Apq[2][0] += p[i].x * q[i].z;
        Apq[2][1] += p[i].y * q[i].z;
        Apq[2][2] += p[i].z * q[i].z;
    }

    mat3 Aqq{0};
    for (int32 i = 0; i < q.count; i++) {
        Aqq[0][0] += q[i].x * q[i].x;
        Aqq[0][1] += q[i].y * q[i].x;
        Aqq[0][2] += q[i].z * q[i].x;
        Aqq[1][0] += q[i].x * q[i].y;
        Aqq[1][1] += q[i].y * q[i].y;
        Aqq[1][2] += q[i].z * q[i].y;
        Aqq[2][0] += q[i].x * q[i].z;
        Aqq[2][1] += q[i].y * q[i].z;
        Aqq[2][2] += q[i].z * q[i].z;
    }

    mat4 result = Apq / Aqq;
    result[3] = vec4(com_b, 1);
    return result;
}

mat4 compute_linear_transform(Array<const vec3> pos_frame_a, Array<const vec3> pos_frame_b, Array<const float> mass) {
    ASSERT(pos_frame_a.count == pos_frame_b.count);
    ASSERT(mass.count == pos_frame_a.count);

    const vec3 com_a = compute_com(pos_frame_a, mass);
    DynamicArray<vec3> q(pos_frame_a.count);
    for (int32 i = 0; i < q.count; i++) {
        q[i] = pos_frame_a[i] - com_a;
    }

    const vec3 com_b = compute_com(pos_frame_b, mass);
    DynamicArray<vec3> p(pos_frame_b.count);
    for (int32 i = 0; i < p.count; i++) {
        p[i] = pos_frame_b[i] - com_b;
    }

    mat3 Apq{0};
    for (int32 i = 0; i < p.count; i++) {
        Apq[0][0] += mass[i] * p[i].x * q[i].x;
        Apq[0][1] += mass[i] * p[i].y * q[i].x;
        Apq[0][2] += mass[i] * p[i].z * q[i].x;
        Apq[1][0] += mass[i] * p[i].x * q[i].y;
        Apq[1][1] += mass[i] * p[i].y * q[i].y;
        Apq[1][2] += mass[i] * p[i].z * q[i].y;
        Apq[2][0] += mass[i] * p[i].x * q[i].z;
        Apq[2][1] += mass[i] * p[i].y * q[i].z;
        Apq[2][2] += mass[i] * p[i].z * q[i].z;
    }

    mat3 Aqq{0};
    for (int32 i = 0; i < q.count; i++) {
        Aqq[0][0] += mass[i] * q[i].x * q[i].x;
        Aqq[0][1] += mass[i] * q[i].y * q[i].x;
        Aqq[0][2] += mass[i] * q[i].z * q[i].x;
        Aqq[1][0] += mass[i] * q[i].x * q[i].y;
        Aqq[1][1] += mass[i] * q[i].y * q[i].y;
        Aqq[1][2] += mass[i] * q[i].z * q[i].y;
        Aqq[2][0] += mass[i] * q[i].x * q[i].z;
        Aqq[2][1] += mass[i] * q[i].y * q[i].z;
        Aqq[2][2] += mass[i] * q[i].z * q[i].z;
    }

    mat4 result = Apq / Aqq;
    result[3] = vec4(com_b, 1);
    return result;
}

void compute_RS(mat3* R, mat3* S, Array<const vec3> x0, Array<const vec3> x, Array<const float> m) {
    ASSERT(x0.count == x.count);
    ASSERT(m.count == x0.count);

    const vec3 com_x0 = compute_com(x0, m);
    DynamicArray<vec3> q(x0.count);
    for (int32 i = 0; i < q.count; i++) {
        q[i] = x0[i] - com_x0;
    }

    const vec3 com_x = compute_com(x, m);
    DynamicArray<vec3> p(x.count);
    for (int32 i = 0; i < p.count; i++) {
        p[i] = x[i] - com_x;
    }

    mat3 Apq{0};
    for (int32 i = 0; i < p.count; i++) {
        Apq[0][0] += m[i] * p[i].x * q[i].x;
        Apq[0][1] += m[i] * p[i].y * q[i].x;
        Apq[0][2] += m[i] * p[i].z * q[i].x;
        Apq[1][0] += m[i] * p[i].x * q[i].y;
        Apq[1][1] += m[i] * p[i].y * q[i].y;
        Apq[1][2] += m[i] * p[i].z * q[i].y;
        Apq[2][0] += m[i] * p[i].x * q[i].z;
        Apq[2][1] += m[i] * p[i].y * q[i].z;
        Apq[2][2] += m[i] * p[i].z * q[i].z;
    }

    mat3 Q, D;
    diagonalize(math::transpose(Apq) * Apq, &Q, &D);
    D[0][0] = sqrtf(D[0][0]);
    D[1][1] = sqrtf(D[1][1]);
    D[2][2] = sqrtf(D[2][2]);

    *S = Q * D * math::inverse(Q);
    *R = Apq * math::inverse(*S);
}

// from here https://stackoverflow.com/questions/4372224/fast-method-for-computing-3x3-symmetric-matrix-spectral-decomposition
// Slightly modified version of  Stan Melax's code for 3x3 matrix diagonalization (Thanks Stan!)
// source: http://www.melax.com/diag.html?attredirects=0
void Diagonalize(const float (&A)[3][3], float (&Q)[3][3], float (&D)[3][3]) {
    // A must be a symmetric matrix.
    // returns Q and D such that
    // Diagonal matrix D = QT * A * Q;  and  A = Q*D*QT
    const int maxsteps = 24;  // certainly wont need that many.
    int k0, k1, k2;
    float o[3], m[3];
    float q[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float jr[4];
    float sqw, sqx, sqy, sqz;
    float tmp1, tmp2, mq;
    float AQ[3][3];
    float thet, sgn, t, c;
    for (int i = 0; i < maxsteps; ++i) {
        // quat to matrix
        sqx = q[0] * q[0];
        sqy = q[1] * q[1];
        sqz = q[2] * q[2];
        sqw = q[3] * q[3];
        Q[0][0] = (sqx - sqy - sqz + sqw);
        Q[1][1] = (-sqx + sqy - sqz + sqw);
        Q[2][2] = (-sqx - sqy + sqz + sqw);
        tmp1 = q[0] * q[1];
        tmp2 = q[2] * q[3];
        Q[1][0] = 2.0f * (tmp1 + tmp2);
        Q[0][1] = 2.0f * (tmp1 - tmp2);
        tmp1 = q[0] * q[2];
        tmp2 = q[1] * q[3];
        Q[2][0] = 2.0f * (tmp1 - tmp2);
        Q[0][2] = 2.0f * (tmp1 + tmp2);
        tmp1 = q[1] * q[2];
        tmp2 = q[0] * q[3];
        Q[2][1] = 2.0f * (tmp1 + tmp2);
        Q[1][2] = 2.0f * (tmp1 - tmp2);

        // AQ = A * Q
        AQ[0][0] = Q[0][0] * A[0][0] + Q[1][0] * A[0][1] + Q[2][0] * A[0][2];
        AQ[0][1] = Q[0][1] * A[0][0] + Q[1][1] * A[0][1] + Q[2][1] * A[0][2];
        AQ[0][2] = Q[0][2] * A[0][0] + Q[1][2] * A[0][1] + Q[2][2] * A[0][2];
        AQ[1][0] = Q[0][0] * A[0][1] + Q[1][0] * A[1][1] + Q[2][0] * A[1][2];
        AQ[1][1] = Q[0][1] * A[0][1] + Q[1][1] * A[1][1] + Q[2][1] * A[1][2];
        AQ[1][2] = Q[0][2] * A[0][1] + Q[1][2] * A[1][1] + Q[2][2] * A[1][2];
        AQ[2][0] = Q[0][0] * A[0][2] + Q[1][0] * A[1][2] + Q[2][0] * A[2][2];
        AQ[2][1] = Q[0][1] * A[0][2] + Q[1][1] * A[1][2] + Q[2][1] * A[2][2];
        AQ[2][2] = Q[0][2] * A[0][2] + Q[1][2] * A[1][2] + Q[2][2] * A[2][2];
        // D = Qt * AQ
        D[0][0] = AQ[0][0] * Q[0][0] + AQ[1][0] * Q[1][0] + AQ[2][0] * Q[2][0];
        D[0][1] = AQ[0][0] * Q[0][1] + AQ[1][0] * Q[1][1] + AQ[2][0] * Q[2][1];
        D[0][2] = AQ[0][0] * Q[0][2] + AQ[1][0] * Q[1][2] + AQ[2][0] * Q[2][2];
        D[1][0] = AQ[0][1] * Q[0][0] + AQ[1][1] * Q[1][0] + AQ[2][1] * Q[2][0];
        D[1][1] = AQ[0][1] * Q[0][1] + AQ[1][1] * Q[1][1] + AQ[2][1] * Q[2][1];
        D[1][2] = AQ[0][1] * Q[0][2] + AQ[1][1] * Q[1][2] + AQ[2][1] * Q[2][2];
        D[2][0] = AQ[0][2] * Q[0][0] + AQ[1][2] * Q[1][0] + AQ[2][2] * Q[2][0];
        D[2][1] = AQ[0][2] * Q[0][1] + AQ[1][2] * Q[1][1] + AQ[2][2] * Q[2][1];
        D[2][2] = AQ[0][2] * Q[0][2] + AQ[1][2] * Q[1][2] + AQ[2][2] * Q[2][2];
        o[0] = D[1][2];
        o[1] = D[0][2];
        o[2] = D[0][1];
        m[0] = fabs(o[0]);
        m[1] = fabs(o[1]);
        m[2] = fabs(o[2]);

        k0 = (m[0] > m[1] && m[0] > m[2]) ? 0 : (m[1] > m[2]) ? 1 : 2;  // index of largest element of offdiag
        k1 = (k0 + 1) % 3;
        k2 = (k0 + 2) % 3;
        if (o[k0] == 0.0f) {
            break;  // diagonal already
        }
        thet = (D[k2][k2] - D[k1][k1]) / (2.0f * o[k0]);
        sgn = (thet > 0.0f) ? 1.0f : -1.0f;
        thet *= sgn;                                                             // make it positive
        t = sgn / (thet + ((thet < 1.E6f) ? sqrtf(thet * thet + 1.0f) : thet));  // sign(T)/(|T|+sqrt(T^2+1))
        c = 1.0f / sqrtf(t * t + 1.0f);                                          //  c= 1/(t^2+1) , t=s/c
        if (c == 1.0f) {
            break;  // no room for improvement - reached machine precision.
        }
        jr[0] = jr[1] = jr[2] = jr[3] = 0.0f;
        jr[k0] = sgn * sqrtf((1.0f - c) / 2.0f);  // using 1/2 angle identity sin(a/2) = sqrt((1-cos(a))/2)
        jr[k0] *= -1.0f;                          // since our quat-to-matrix convention was for v*M instead of M*v
        jr[3] = sqrtf(1.0f - jr[k0] * jr[k0]);
        if (jr[3] == 1.0f) {
            break;  // reached limits of floating point precision
        }
        q[0] = (q[3] * jr[0] + q[0] * jr[3] + q[1] * jr[2] - q[2] * jr[1]);
        q[1] = (q[3] * jr[1] - q[0] * jr[2] + q[1] * jr[3] + q[2] * jr[0]);
        q[2] = (q[3] * jr[2] + q[0] * jr[1] - q[1] * jr[0] + q[2] * jr[3]);
        q[3] = (q[3] * jr[3] - q[0] * jr[0] - q[1] * jr[1] - q[2] * jr[2]);
        mq = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        q[0] /= mq;
        q[1] /= mq;
        q[2] /= mq;
        q[3] /= mq;
    }
}

void diagonalize(const mat3& M, mat3* Q, mat3* D) {
    ASSERT(Q);
    ASSERT(D);
    Diagonalize((const float(&)[3][3])M, (float(&)[3][3]) * Q, (float(&)[3][3]) * D);
}

void decompose(const mat3& M, mat3* R, mat3* S) {
    ASSERT(R);
    ASSERT(S);
    mat3 Q, D;
    mat3 AtA = math::transpose(M) * M;
    diagonalize(AtA, &Q, &D);
    // float det = math::determinant(AtA);
    D[0][0] = sqrtf(D[0][0]);
    D[1][1] = sqrtf(D[1][1]);
    D[2][2] = sqrtf(D[2][2]);
    *S = math::inverse(Q) * D * Q;
    *R = M * math::inverse(*S);
}
