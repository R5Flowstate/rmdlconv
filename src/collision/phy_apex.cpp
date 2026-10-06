//=============================================================================//
// Valve/IVP (id=0) .phy -> Apex geoms (id=1) .phy
//=============================================================================//

#include "phy_apex.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <numeric>

namespace collision
{
    namespace
    {
        // IVP metres (y down) -> model inches.
        constexpr double kIvpToInches = 39.3701;

        // Adjacent triangles fold into one side only when the far vertex is on the shared
        // plane; nearly-coplanar pairs stay split so every side is exactly planar.
        constexpr double kCoplanarDist = 1e-3;
        constexpr double kCoplanarDot = 0.99;

        struct V3 { double x, y, z; };
        inline V3 sub(const V3& a, const V3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
        inline V3 add(const V3& a, const V3& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
        inline V3 mul(const V3& a, double s) { return { a.x * s, a.y * s, a.z * s }; }
        inline double dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
        inline V3 cross(const V3& a, const V3& b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
        inline double len(const V3& a) { return std::sqrt(dot(a, a)); }
        inline V3 norm(const V3& a) { const double l = len(a); return l > 0 ? mul(a, 1.0 / l) : V3{ 0, 0, 0 }; }

        template <typename T> T rd(const uint8_t* p) { T v; memcpy(&v, p, sizeof(T)); return v; }

        struct Ledge
        {
            std::vector<V3> points;               // ledge-local, already in model inches
            std::vector<std::array<int, 3>> tris;
        };

        bool BuildHull(const Ledge& ledge, ApexHull& out, std::string& err)
        {
            // Compact to the vertices the triangles use.
            std::vector<int> used;
            for (const auto& t : ledge.tris)
                for (int k : t) used.push_back(k);
            std::sort(used.begin(), used.end());
            used.erase(std::unique(used.begin(), used.end()), used.end());
            if (used.size() < 4 || used.size() > 255)
            {
                err = "hull vertex count out of range";
                return false;
            }
            std::map<int, int> remap;
            std::vector<V3> P;
            for (int u : used) { remap[u] = static_cast<int>(P.size()); P.push_back(ledge.points[u]); }

            V3 c0{ 0, 0, 0 };
            for (const auto& p : P) c0 = add(c0, p);
            c0 = mul(c0, 1.0 / P.size());

            struct Tri { std::array<int, 3> v; V3 n; double d; double area; };
            std::vector<Tri> tris;
            for (const auto& t : ledge.tris)
            {
                Tri tr{ { remap[t[0]], remap[t[1]], remap[t[2]] } };
                V3 n = cross(sub(P[tr.v[1]], P[tr.v[0]]), sub(P[tr.v[2]], P[tr.v[0]]));
                if (dot(n, sub(P[tr.v[0]], c0)) < 0) { std::swap(tr.v[1], tr.v[2]); n = mul(n, -1.0); }
                tr.area = len(n);
                if (tr.area <= 0) continue;
                tr.n = norm(n);
                tr.d = dot(tr.n, P[tr.v[0]]);
                tris.push_back(tr);
            }

            std::vector<int> parent(tris.size());
            std::iota(parent.begin(), parent.end(), 0);
            auto find = [&](int x) { while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; } return x; };
            for (size_t i = 0; i < tris.size(); i++)
            {
                for (size_t j = i + 1; j < tris.size(); j++)
                {
                    int shared = 0, far = -1;
                    for (int a : tris[j].v)
                    {
                        if (std::find(tris[i].v.begin(), tris[i].v.end(), a) != tris[i].v.end()) shared++;
                        else far = a;
                    }
                    if (shared != 2 || dot(tris[i].n, tris[j].n) < kCoplanarDot)
                        continue;
                    if (std::abs(dot(tris[i].n, P[far]) - tris[i].d) < kCoplanarDist)
                        parent[find(static_cast<int>(j))] = find(static_cast<int>(i));
                }
            }

            std::map<int, std::vector<int>> groups;
            for (size_t i = 0; i < tris.size(); i++) groups[find(static_cast<int>(i))].push_back(static_cast<int>(i));

            // A merged group must be planar; a curved run joined edge by edge is not, so it falls
            // back to one side per triangle.
            std::vector<std::vector<int>> faces;
            for (const auto& [root, members] : groups)
            {
                V3 n{ 0, 0, 0 };
                for (int m : members) n = add(n, mul(tris[m].n, tris[m].area));
                n = norm(n);
                double lo = 1e30, hi = -1e30;
                for (int m : members)
                    for (int k : tris[m].v) { const double d = dot(n, P[k]); lo = std::min(lo, d); hi = std::max(hi, d); }
                if (members.size() == 1 || hi - lo < kCoplanarDist)
                    faces.push_back(members);
                else
                    for (int m : members) faces.push_back({ m });
            }

            // Each side's loop is its boundary: the triangle edges not shared inside the face, walked
            // head to tail. Neighbouring loops then share exactly the same edges.
            struct Side { std::vector<int> v; V3 n; double d; };
            std::vector<Side> sides;
            for (const auto& members : faces)
            {
                std::map<std::pair<int, int>, int> directed;
                for (int m : members)
                    for (int k = 0; k < 3; k++) directed[{ tris[m].v[k], tris[m].v[(k + 1) % 3] }]++;
                std::map<int, int> next;
                for (const auto& [e, c] : directed)
                {
                    if (directed.count({ e.second, e.first }))
                        continue;
                    if (next.count(e.first)) { err = "side boundary branches"; return false; }
                    next[e.first] = e.second;
                }
                if (next.empty()) { err = "side has no boundary"; return false; }
                std::vector<int> vs;
                int at = next.begin()->first;
                do
                {
                    vs.push_back(at);
                    auto it = next.find(at);
                    if (it == next.end() || vs.size() > next.size()) { err = "side boundary is not one loop"; return false; }
                    at = it->second;
                } while (at != vs.front());
                if (vs.size() != next.size()) { err = "side boundary is not one loop"; return false; }

                V3 n{ 0, 0, 0 };
                for (int m : members) n = add(n, mul(tris[m].n, tris[m].area));
                n = norm(n);

                // Start at the smallest angle about the centre, measured from the lowest-index vertex.
                std::vector<int> byIndex = vs;
                std::sort(byIndex.begin(), byIndex.end());
                V3 ctr{ 0, 0, 0 };
                for (int k : byIndex) ctr = add(ctr, P[k]);
                ctr = mul(ctr, 1.0 / vs.size());
                const V3 u = norm(sub(P[byIndex[0]], ctr));
                const V3 w = cross(n, u);
                auto angle = [&](int k) { const V3 dk = sub(P[k], ctr); return std::atan2(dot(dk, w), dot(dk, u)); };
                std::rotate(vs.begin(), std::min_element(vs.begin(), vs.end(), [&](int a, int b) { return angle(a) < angle(b); }), vs.end());
                if (vs.size() > 32)
                {
                    err = "side has more than 32 vertices";
                    return false;
                }
                double d = -1e30;
                for (int k : vs) d = std::max(d, dot(n, P[k]));
                sides.push_back({ vs, n, d });
            }

            // Vertices inside a merged face are not hull corners.
            {
                std::vector<int> keep(P.size(), -1);
                for (const auto& s : sides)
                    for (int k : s.v) keep[k] = 0;
                std::vector<V3> Q;
                for (size_t k = 0; k < P.size(); k++)
                    if (keep[k] == 0) { keep[k] = static_cast<int>(Q.size()); Q.push_back(P[k]); }
                for (auto& s : sides)
                    for (int& k : s.v) k = keep[k];
                P.swap(Q);
            }

            // Edges in first-seen order while walking the sides; side A walks v0->v1.
            std::map<std::pair<int, int>, size_t> seen;
            std::vector<std::array<int, 4>> edges;
            for (size_t s = 0; s < sides.size(); s++)
            {
                const auto& v = sides[s].v;
                for (size_t k = 0; k < v.size(); k++)
                {
                    const int a = v[k], b = v[(k + 1) % v.size()];
                    auto it = seen.find({ b, a });
                    if (it != seen.end()) edges[it->second][3] = static_cast<int>(s);
                    else { seen[{ a, b }] = edges.size(); edges.push_back({ a, b, static_cast<int>(s), -1 }); }
                }
            }
            for (const auto& e : edges)
            {
                if (e[3] < 0) { err = "open edge in hull"; return false; }
            }
            if (static_cast<int>(P.size()) - static_cast<int>(edges.size()) + static_cast<int>(sides.size()) != 2)
            {
                err = "hull fails Euler characteristic";
                return false;
            }

            // Mass properties by tetrahedra fanned from c0.
            double vol = 0;
            V3 C{ 0, 0, 0 };
            struct Tet { double v; V3 p[4]; };
            std::vector<Tet> tets;
            for (const auto& s : sides)
            {
                for (size_t k = 1; k + 1 < s.v.size(); k++)
                {
                    const V3 a = P[s.v[0]], b = P[s.v[k]], c = P[s.v[k + 1]];
                    const double tv = dot(sub(a, c0), cross(sub(b, c0), sub(c, c0))) / 6.0;
                    tets.push_back({ tv, { c0, a, b, c } });
                    vol += tv;
                    C = add(C, mul(add(add(c0, a), add(b, c)), tv / 4.0));
                }
            }
            if (vol <= 0) { err = "hull has no volume"; return false; }
            C = mul(C, 1.0 / vol);

            double cov[3][3] = {};
            for (const auto& t : tets)
            {
                V3 q[4], s{ 0, 0, 0 };
                for (int k = 0; k < 4; k++) { q[k] = sub(t.p[k], C); s = add(s, q[k]); }
                const double sv[3] = { s.x, s.y, s.z };
                for (int i = 0; i < 3; i++)
                {
                    for (int j = 0; j < 3; j++)
                    {
                        double qq = 0;
                        for (int k = 0; k < 4; k++)
                        {
                            const double qi[3] = { q[k].x, q[k].y, q[k].z };
                            qq += qi[i] * qi[j];
                        }
                        cov[i][j] += (sv[i] * sv[j] + qq) * t.v / 20.0;
                    }
                }
            }
            const double tr = cov[0][0] + cov[1][1] + cov[2][2];
            const double cv[3] = { C.x, C.y, C.z };
            const double cc = dot(C, C);
            for (int i = 0; i < 3; i++)
            {
                for (int j = 0; j < 3; j++)
                {
                    const double ic = ((i == j ? tr : 0.0) - cov[i][j]) / vol;
                    out.inertiaAtOrigin[i][j] = ic + (i == j ? cc : 0.0) - cv[i] * cv[j];
                }
            }

            double inscribed = 1e30;
            for (const auto& s : sides) inscribed = std::min(inscribed, std::abs(dot(s.n, C) - s.d));

            out.verts.clear();
            for (int k = 0; k < 3; k++) { out.bbMin[k] = 1e30f; out.bbMax[k] = -1e30f; }
            for (const auto& p : P)
            {
                const float f[3] = { static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z) };
                out.verts.push_back({ f[0], f[1], f[2] });
                for (int k = 0; k < 3; k++) { out.bbMin[k] = std::min(out.bbMin[k], f[k]); out.bbMax[k] = std::max(out.bbMax[k], f[k]); }
            }
            out.sides.clear();
            for (const auto& s : sides)
            {
                std::vector<uint8_t> v;
                for (int k : s.v) v.push_back(static_cast<uint8_t>(k));
                out.sides.push_back(v);
            }
            out.edges.clear();
            for (const auto& e : edges)
                out.edges.push_back({ static_cast<uint8_t>(e[0]), static_cast<uint8_t>(e[1]), static_cast<uint8_t>(e[2]), static_cast<uint8_t>(e[3]) });
            out.volume = vol;
            out.centroid[0] = C.x; out.centroid[1] = C.y; out.centroid[2] = C.z;
            out.inscribedRadius = static_cast<float>(inscribed);
            return true;
        }

        // "solid { "index" "N" "name" "..." "surfaceprop" "..." }" -> per-index name/surfaceprop.
        void ReadSolidKeyValues(const std::string& kv, std::vector<ApexSolid>& solids)
        {
            auto value = [](const std::string& block, const char* key) -> std::string {
                const std::string k = std::string("\"") + key + "\" \"";
                const size_t at = block.find(k);
                if (at == std::string::npos) return {};
                const size_t s = at + k.size();
                const size_t e = block.find('"', s);
                return e == std::string::npos ? std::string() : block.substr(s, e - s);
            };
            size_t pos = 0;
            while ((pos = kv.find("solid {", pos)) != std::string::npos)
            {
                const size_t end = kv.find('}', pos);
                if (end == std::string::npos) break;
                const std::string block = kv.substr(pos, end - pos);
                const std::string idx = value(block, "index");
                if (!idx.empty())
                {
                    const size_t i = static_cast<size_t>(std::stoi(idx));
                    if (i < solids.size())
                    {
                        solids[i].boneName = value(block, "name");
                        solids[i].surfaceProp = value(block, "surfaceprop");
                    }
                }
                pos = end;
            }
        }

        // Apex copies a break "model" value verbatim as the gib's model name and only loads
        // names ending in .rmdl, so Valve's bare "dir\name" form becomes "mdl/dir/name.rmdl".
        void CanonicalizeBreakModels(std::string& kv)
        {
            static const std::string kKey = "\"model\"";
            size_t pos = 0;
            while ((pos = kv.find("break", pos)) != std::string::npos)
            {
                const size_t open = kv.find('{', pos);
                const size_t close = open == std::string::npos ? open : kv.find('}', open);
                if (close == std::string::npos) break;
                const size_t key = kv.find(kKey, open);
                if (key == std::string::npos || key > close) { pos = close; continue; }
                const size_t vs = kv.find('"', key + kKey.size());
                const size_t ve = vs == std::string::npos ? vs : kv.find('"', vs + 1);
                if (ve == std::string::npos || ve > close) { pos = close; continue; }

                std::string name = kv.substr(vs + 1, ve - vs - 1);
                std::replace(name.begin(), name.end(), '\\', '/');
                std::transform(name.begin(), name.end(), name.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                auto endsWith = [&](const char* s) {
                    const size_t n = std::strlen(s);
                    return name.size() >= n && name.compare(name.size() - n, n, s) == 0;
                };
                if (name.rfind("models/", 0) == 0) name.erase(0, 7);
                if (endsWith(".mdl")) name.erase(name.size() - 4);
                if (!endsWith(".rmdl"))
                {
                    if (name.rfind("mdl/", 0) != 0) name.insert(0, "mdl/");
                    name += ".rmdl";
                }
                printf("  break model -> %s\n", name.c_str());
                kv.replace(vs + 1, ve - vs - 1, name);
                pos = kv.find('}', vs + 1 + name.size());
                if (pos == std::string::npos) break;
            }
        }
    }

    ApexPhy BuildApexPhyFromValve(const void* phyData, size_t phySize)
    {
        ApexPhy out;
        const uint8_t* const base = static_cast<const uint8_t*>(phyData);
        const uint8_t* const end = base + phySize;
        if (phySize < 16)
        {
            out.error = "phy too small";
            return out;
        }

        const int32_t hdrSize = rd<int32_t>(base);
        const int32_t id = rd<int32_t>(base + 4);
        const int32_t solidCount = rd<int32_t>(base + 8);
        out.checksum = rd<int32_t>(base + 12);
        if ((hdrSize != 16 && hdrSize != 20) || id != 0 || solidCount <= 0 || solidCount > 255)
        {
            out.error = "not a Valve/IVP phy";
            return out;
        }

        // swapcompactsurfaceheader_t (32) + legacysurfaceheader_t (48), then compact ledges.
        constexpr size_t kLedgesStart = 32 + 48;
        const uint8_t* p = base + hdrSize;
        for (int s = 0; s < solidCount; s++)
        {
            if (p + kLedgesStart + 16 > end)
            {
                out.error = "solid " + std::to_string(s) + " out of bounds";
                return out;
            }
            const uint8_t* const solidEnd = p + 4 + rd<int32_t>(p);
            const uint8_t* t = p + kLedgesStart;
            const uint8_t* const pointsStart = t + rd<int32_t>(t);

            ApexSolid solid;
            while (t + 16 <= pointsStart)
            {
                const int32_t ledgePoints = rd<int32_t>(t);
                const int32_t numFaces = rd<int32_t>(t + 12);
                if (numFaces <= 0)
                    break;

                Ledge ledge;
                int maxIndex = -1;
                for (int f = 0; f < numFaces; f++)
                {
                    const uint8_t* const face = t + 16 + 16 * f;
                    std::array<int, 3> tri = { rd<int16_t>(face + 4), rd<int16_t>(face + 8), rd<int16_t>(face + 12) };
                    for (int k : tri) maxIndex = std::max(maxIndex, k);
                    ledge.tris.push_back(tri);
                }
                const uint8_t* const pts = t + ledgePoints;
                if (maxIndex < 0 || pts + 16 * (maxIndex + 1) > solidEnd)
                {
                    out.error = "ledge points out of bounds in solid " + std::to_string(s);
                    return out;
                }
                for (int i = 0; i <= maxIndex; i++)
                {
                    const float x = rd<float>(pts + 16 * i), y = rd<float>(pts + 16 * i + 4), z = rd<float>(pts + 16 * i + 8);
                    ledge.points.push_back({ x * kIvpToInches, z * kIvpToInches, -y * kIvpToInches });
                }

                ApexHull hull;
                std::string err;
                if (!BuildHull(ledge, hull, err))
                {
                    out.error = "solid " + std::to_string(s) + ": " + err;
                    return out;
                }
                solid.hulls.push_back(std::move(hull));
                t += 16 + 16 * numFaces;
            }
            if (solid.hulls.empty())
            {
                out.error = "solid " + std::to_string(s) + " has no ledges";
                return out;
            }
            out.solids.push_back(std::move(solid));
            p = solidEnd;
        }

        if (p < end)
            out.keyValues.assign(reinterpret_cast<const char*>(p), end - p);
        CanonicalizeBreakModels(out.keyValues);
        ReadSolidKeyValues(out.keyValues, out.solids);
        out.valid = true;
        return out;
    }

    std::vector<uint8_t> WriteApexPhy(const ApexPhy& phy)
    {
        constexpr size_t kHeader = 20, kPtrHeader = 32, kGroup = 144, kSolid = 64;
        std::vector<uint8_t> out(kHeader + kPtrHeader + kGroup * phy.solids.size(), 0);
        auto put = [&](size_t off, const void* v, size_t n) { memcpy(out.data() + off, v, n); };
        auto put32 = [&](size_t off, int32_t v) { put(off, &v, 4); };
        auto put64 = [&](size_t off, int64_t v) { put(off, &v, 8); };
        auto putf = [&](size_t off, float v) { put(off, &v, 4); };

        const size_t ptr = kHeader;
        put64(ptr + 0, static_cast<int64_t>(kPtrHeader));
        put64(ptr + 8, static_cast<int64_t>(phy.solids.size()));

        for (size_t g = 0; g < phy.solids.size(); g++)
        {
            const ApexSolid& solid = phy.solids[g];
            const size_t grp = ptr + kPtrHeader + g * kGroup;

            double vol = 0, C[3] = {}, I[3][3] = {};
            float bbMin[3] = { 1e30f, 1e30f, 1e30f }, bbMax[3] = { -1e30f, -1e30f, -1e30f };
            for (const auto& h : solid.hulls)
            {
                vol += h.volume;
                for (int i = 0; i < 3; i++)
                {
                    C[i] += h.centroid[i] * h.volume;
                    bbMin[i] = std::min(bbMin[i], h.bbMin[i]);
                    bbMax[i] = std::max(bbMax[i], h.bbMax[i]);
                    for (int j = 0; j < 3; j++) I[i][j] += h.inertiaAtOrigin[i][j] * h.volume;
                }
            }
            for (int i = 0; i < 3; i++)
            {
                C[i] /= vol;
                for (int j = 0; j < 3; j++) I[i][j] /= vol;
            }
            double maxR2 = 0;
            for (const auto& h : solid.hulls)
            {
                for (const auto& v : h.verts)
                {
                    const double dx = v[0] - C[0], dy = v[1] - C[1], dz = v[2] - C[2];
                    maxR2 = std::max(maxR2, dx * dx + dy * dy + dz * dz);
                }
            }

            const size_t firstSolid = out.size();
            put64(grp + 0, static_cast<int64_t>(firstSolid - ptr));
            put64(grp + 8, static_cast<int64_t>(solid.hulls.size()));
            putf(grp + 16, 1.0f);
            for (int i = 0; i < 3; i++) putf(grp + 32 + 4 * i, static_cast<float>(C[i]));
            for (int r = 0; r < 3; r++)
                for (int i = 0; i < 3; i++) putf(grp + 48 + 16 * r + 4 * i, static_cast<float>(I[r][i]));
            for (int i = 0; i < 3; i++) putf(grp + 96 + 4 * i, static_cast<float>(C[i]));
            put32(grp + 108, static_cast<int32_t>(std::floor(maxR2)));
            for (int i = 0; i < 3; i++) { putf(grp + 112 + 4 * i, bbMin[i]); putf(grp + 124 + 4 * i, bbMax[i]); }

            out.resize(out.size() + kSolid * solid.hulls.size(), 0);
            for (size_t hIdx = 0; hIdx < solid.hulls.size(); hIdx++)
            {
                const ApexHull& h = solid.hulls[hIdx];
                const size_t so = firstSolid + hIdx * kSolid;
                for (int i = 0; i < 3; i++) putf(so + 4 * i, static_cast<float>(h.centroid[i]));
                putf(so + 12, h.inscribedRadius);

                const size_t vOff = out.size();
                for (const auto& v : h.verts)
                {
                    const size_t at = out.size();
                    out.resize(at + 12);
                    put(at, v.data(), 12);
                }
                const size_t sOff = out.size();
                for (const auto& s : h.sides)
                {
                    const size_t at = out.size();
                    out.resize(at + 32, 0xFF);
                    put(at, s.data(), s.size());
                }
                const size_t eOff = out.size();
                for (const auto& e : h.edges)
                {
                    const size_t at = out.size();
                    out.resize(at + 4);
                    put(at, e.data(), 4);
                }

                put64(so + 16, static_cast<int64_t>(vOff - ptr));
                put64(so + 24, static_cast<int64_t>(h.verts.size()));
                put64(so + 32, static_cast<int64_t>(sOff - ptr));
                put64(so + 40, static_cast<int64_t>(h.sides.size()));
                put64(so + 48, static_cast<int64_t>(eOff - ptr));
                put64(so + 56, static_cast<int64_t>(h.edges.size()));
            }
        }

        put64(ptr + 24, static_cast<int64_t>(out.size() - ptr));
        put32(0, static_cast<int32_t>(kHeader));
        put32(4, 1);
        put32(8, static_cast<int32_t>(phy.solids.size()));
        put32(12, phy.checksum);
        put32(16, static_cast<int32_t>(out.size()));
        out.insert(out.end(), phy.keyValues.begin(), phy.keyValues.end());
        return out;
    }

    void TriangulateHull(const ApexHull& hull, std::vector<std::array<uint32_t, 3>>& outTris)
    {
        for (const auto& s : hull.sides)
            for (size_t k = 1; k + 1 < s.size(); k++)
                outTris.push_back({ s[0], s[k], s[k + 1] });
    }
}
