// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from bbhost src/engine/esd.cpp at 7c790536.
#include "esd.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <utility>

namespace esd {

namespace {

constexpr std::size_t kBase = 0x6c;  // the data block: every offset counts from here

struct Reader {
    const std::vector<std::uint8_t>& d;
    bool ok = true;
    std::int32_t i32(std::int64_t at) {
        if (at < 0 || static_cast<std::uint64_t>(at) + 4 > d.size()) {
            ok = false;
            return 0;
        }
        std::int32_t v;
        std::memcpy(&v, d.data() + at, 4);
        return v;
    }
    std::int64_t q(std::int64_t rel) {  // a data-block field
        const std::int64_t at = static_cast<std::int64_t>(kBase) + rel;
        if (rel < 0 || static_cast<std::uint64_t>(at) + 8 > d.size()) {
            ok = false;
            return 0;
        }
        std::int64_t v;
        std::memcpy(&v, d.data() + at, 8);
        return v;
    }
    std::vector<std::uint8_t> bytes(std::int64_t rel, std::int64_t n) {
        const std::int64_t at = static_cast<std::int64_t>(kBase) + rel;
        if (rel < 0 || n < 0 || static_cast<std::uint64_t>(at + n) > d.size()) {
            ok = false;
            return {};
        }
        return std::vector<std::uint8_t>(d.begin() + at, d.begin() + at + n);
    }
};

using Key = std::pair<std::int64_t, std::int64_t>;

struct Parser {
    Reader r;
    Script& s;
    std::map<Key, int> expr_memo, arg_memo, cmd_memo, condrun_memo;
    std::map<std::int64_t, int> cond_memo, state_at;
    int depth = 0;

    int expr(std::int64_t off, std::int64_t n) {
        const auto it = expr_memo.find({off, n});
        if (it != expr_memo.end()) return it->second;
        s.exprs.push_back({r.bytes(off, n), off});
        return expr_memo[{off, n}] = static_cast<int>(s.exprs.size() - 1);
    }
    int argrun(std::int64_t off, std::int64_t n) {
        const auto it = arg_memo.find({off, n});
        if (it != arg_memo.end()) return it->second;
        Run run{{}, n ? off : kNew, off};
        for (std::int64_t j = 0; j < n && r.ok; ++j) run.items.push_back(expr(r.q(off + 0x10 * j), r.q(off + 0x10 * j + 8)));
        s.runs.push_back(std::move(run));
        return arg_memo[{off, n}] = static_cast<int>(s.runs.size() - 1);
    }
    int cmdrun(std::int64_t off, std::int64_t n) {
        const auto it = cmd_memo.find({off, n});
        if (it != cmd_memo.end()) return it->second;
        Run run{{}, n ? off : kNew, off};
        for (std::int64_t j = 0; j < n && r.ok; ++j) {
            const std::int64_t c = off + 0x18 * j;
            Cmd cm;
            cm.bank = r.i32(static_cast<std::int64_t>(kBase) + c);
            cm.id = r.i32(static_cast<std::int64_t>(kBase) + c + 4);
            cm.args = argrun(r.q(c + 8), r.q(c + 16));
            s.cmds.push_back(cm);
            run.items.push_back(static_cast<int>(s.cmds.size() - 1));
        }
        s.runs.push_back(std::move(run));
        return cmd_memo[{off, n}] = static_cast<int>(s.runs.size() - 1);
    }
    int cond(std::int64_t off) {
        const auto it = cond_memo.find(off);
        if (it != cond_memo.end()) return it->second;
        if (++depth > 64) {
            r.ok = false;
            return 0;
        }
        const std::int64_t tgt = r.q(off), po = r.q(off + 8), pc = r.q(off + 16), so = r.q(off + 24), sc = r.q(off + 32), eo = r.q(off + 40),
                           el = r.q(off + 48);
        const int idx = static_cast<int>(s.conds.size());
        s.conds.push_back({});
        cond_memo[off] = idx;
        int target = -1;
        if (tgt >= 0) {
            const auto st = state_at.find(tgt);
            if (st == state_at.end()) r.ok = false;
            else target = st->second;
        }
        const int ev = expr(eo, el);
        const int pass = cmdrun(po, pc);
        const int subs = condrun(so, sc);
        s.conds[static_cast<std::size_t>(idx)] = Cond{target, ev, pass, subs, off};
        --depth;
        return idx;
    }
    int condrun(std::int64_t off, std::int64_t n) {
        const auto it = condrun_memo.find({off, n});
        if (it != condrun_memo.end()) return it->second;
        Run run{{}, n ? off : kNew, off};
        for (std::int64_t j = 0; j < n && r.ok; ++j) run.items.push_back(cond(r.q(off + 8 * j)));
        s.runs.push_back(std::move(run));
        return condrun_memo[{off, n}] = static_cast<int>(s.runs.size() - 1);
    }
};

void put64(std::vector<std::uint8_t>& b, std::size_t at, std::int64_t v) { std::memcpy(b.data() + at, &v, 8); }
void put32(std::vector<std::uint8_t>& b, std::size_t at, std::int64_t v) {
    const auto w = static_cast<std::int32_t>(v);
    std::memcpy(b.data() + at, &w, 4);
}

}  // namespace

bool parse(const std::vector<std::uint8_t>& d, Script& s, std::string* why) {
    s = Script{};
    if (d.size() < kBase + 0x48 || std::memcmp(d.data(), "fsSL", 4) != 0) {
        if (why) *why = "not a long-format talk script";
        return false;
    }
    Parser p{Reader{d}, s, {}, {}, {}, {}, {}, {}, 0};
    Reader& r = p.r;
    s.head.assign(d.begin(), d.begin() + kBase);
    s.dhead.assign(d.begin() + kBase, d.begin() + kBase + 0x18);
    s.dtail.assign(d.begin() + kBase + 0x38, d.begin() + kBase + 0x48);
    const std::int64_t groups_off = r.q(0x18), ngroups = r.q(0x20), name_off = r.q(0x28), name_len = r.q(0x30);
    if (ngroups < 0 || ngroups > 100000 || name_len < 0 || name_len > 4096) {
        if (why) *why = "implausible counts";
        return false;
    }
    if (ngroups == 0) s.empty_groups_off = groups_off;
    s.name = r.bytes(name_off, 2 * name_len);
    struct G {
        std::int64_t id, off, n;
    };
    std::vector<G> raw;
    std::int64_t states_total = 0;
    for (std::int64_t g = 0; g < ngroups && r.ok; ++g) {
        const std::int64_t at = groups_off + 0x20 * g;
        const G gr{r.q(at), r.q(at + 8), r.q(at + 16)};
        if (r.q(at + 24) != gr.off || gr.n < 1 || gr.n > 100000) {
            if (why) *why = "a state group that is not one array";
            return false;
        }
        raw.push_back(gr);
        for (std::int64_t k = 0; k <= gr.n; ++k) {  // + the initial state's copy
            p.state_at[gr.off + 0x48 * k] = static_cast<int>(s.states.size());
            s.states.push_back(State{r.q(gr.off + 0x48 * k), -1, -1, -1, -1});
        }
        states_total += gr.n + 1;
    }
    if (r.ok && states_total != r.i32(0x30)) {
        if (why) *why = "the state count does not match the groups";
        return false;
    }
    for (const G& gr : raw) {
        if (!r.ok) break;
        Group grp{gr.id, {}, -1, gr.off};
        for (std::int64_t k = 0; k <= gr.n && r.ok; ++k) {
            const std::int64_t at = gr.off + 0x48 * k;
            const int si = p.state_at[at];
            const int conds = p.condrun(r.q(at + 8), r.q(at + 16));
            const int entry = p.cmdrun(r.q(at + 24), r.q(at + 32));
            const int exit = p.cmdrun(r.q(at + 40), r.q(at + 48));
            const int whiles = p.cmdrun(r.q(at + 56), r.q(at + 64));
            State& st = s.states[static_cast<std::size_t>(si)];
            st.conds = conds, st.entry = entry, st.exit = exit, st.whiles = whiles;
            if (k < gr.n) grp.states.push_back(si);
            else grp.copy = si;
        }
        s.groups.push_back(std::move(grp));
    }
    // Condition-pointer slots no list accounts for: the game's writer repeats
    // each group's initial-state list. Kept as unreferenced lists, in place.
    const std::int64_t co = r.i32(0x4c), con = r.i32(0x50);
    std::set<std::int64_t> covered;
    for (const auto& [k, idx] : p.condrun_memo)
        for (std::int64_t j = 0; j < k.second; ++j) covered.insert(k.first + 8 * j);
    for (std::int64_t at = co; at < co + 8 * con && r.ok;) {
        if (covered.count(at)) {
            at += 8;
            continue;
        }
        std::int64_t end = at;
        while (end < co + 8 * con && !covered.count(end)) end += 8;
        Run dead{{}, at, at};
        for (std::int64_t x = at; x < end && r.ok; x += 8) dead.items.push_back(p.cond(r.q(x)));
        s.runs.push_back(std::move(dead));
        s.dead.push_back(static_cast<int>(s.runs.size() - 1));
        at = end;
    }
    if (!r.ok) {
        if (why) *why = "an offset outside the file";
        return false;
    }
    return true;
}

std::vector<std::uint8_t> serialize(const Script& in) {
    Script s = in;
    std::vector<int> groups(s.groups.size());
    for (std::size_t k = 0; k < groups.size(); ++k) groups[k] = static_cast<int>(k);
    std::stable_sort(groups.begin(), groups.end(), [&](int a, int b) { return s.groups[a].key < s.groups[b].key; });
    for (const int g : groups) {
        Group& grp = s.groups[static_cast<std::size_t>(g)];
        if (grp.copy >= 0 || grp.states.empty()) continue;  // a new group: the copy the game's files carry
        const State first = s.states[static_cast<std::size_t>(grp.states[0])];
        const int c = s.state(first.id);
        s.runs[static_cast<std::size_t>(s.states[static_cast<std::size_t>(c)].conds)].items = s.runs[static_cast<std::size_t>(first.conds)].items;
        grp.copy = c;
    }
    std::vector<int> states;
    for (const int g : groups) {
        for (const int st : s.groups[static_cast<std::size_t>(g)].states) states.push_back(st);
        states.push_back(s.groups[static_cast<std::size_t>(g)].copy);
    }
    std::vector<int> conds;
    std::vector<char> seen(s.conds.size(), 0);
    auto walk = [&](auto&& self, int c) -> void {
        if (seen[static_cast<std::size_t>(c)]) return;
        seen[static_cast<std::size_t>(c)] = 1;
        conds.push_back(c);
        for (const int sc : s.runs[static_cast<std::size_t>(s.conds[static_cast<std::size_t>(c)].subs)].items) self(self, sc);
    };
    for (const int st : states)
        for (const int c : s.runs[static_cast<std::size_t>(s.states[static_cast<std::size_t>(st)].conds)].items) walk(walk, c);
    std::stable_sort(conds.begin(), conds.end(), [&](int a, int b) { return s.conds[a].key < s.conds[b].key; });
    auto uniq = [&](const std::vector<int>& in_runs) {
        std::vector<int> out;
        std::vector<char> in_out(s.runs.size(), 0);
        for (const int r : in_runs) {
            if (r < 0 || s.runs[static_cast<std::size_t>(r)].items.empty() || in_out[static_cast<std::size_t>(r)]) continue;
            in_out[static_cast<std::size_t>(r)] = 1;
            out.push_back(r);
        }
        std::stable_sort(out.begin(), out.end(), [&](int a, int b) { return s.runs[a].key < s.runs[b].key; });
        return out;
    };
    std::vector<int> want;
    for (const int st : states) {
        const State& x = s.states[static_cast<std::size_t>(st)];
        want.push_back(x.entry), want.push_back(x.exit), want.push_back(x.whiles);
    }
    for (const int c : conds) want.push_back(s.conds[static_cast<std::size_t>(c)].pass);
    const std::vector<int> cmdruns = uniq(want);
    std::vector<int> cmds;
    for (const int r : cmdruns)
        for (const int cm : s.runs[static_cast<std::size_t>(r)].items) cmds.push_back(cm);
    want.clear();
    for (const int cm : cmds) want.push_back(s.cmds[static_cast<std::size_t>(cm)].args);
    const std::vector<int> argruns = uniq(want);
    want.clear();
    for (const int st : states) want.push_back(s.states[static_cast<std::size_t>(st)].conds);
    for (const int c : conds) want.push_back(s.conds[static_cast<std::size_t>(c)].subs);
    for (const int d : s.dead) want.push_back(d);
    const std::vector<int> condruns = uniq(want);
    std::vector<int> exprs;
    std::vector<char> expr_seen(s.exprs.size(), 0);
    auto add_expr = [&](int x) {
        if (!expr_seen[static_cast<std::size_t>(x)]) expr_seen[static_cast<std::size_t>(x)] = 1, exprs.push_back(x);
    };
    for (const int r : argruns)
        for (const int x : s.runs[static_cast<std::size_t>(r)].items) add_expr(x);
    for (const int c : conds) add_expr(s.conds[static_cast<std::size_t>(c)].eval);
    std::stable_sort(exprs.begin(), exprs.end(), [&](int a, int b) { return s.exprs[a].key < s.exprs[b].key; });

    std::vector<std::int64_t> state_pos(s.states.size(), -1), cond_pos(s.conds.size(), -1), run_pos(s.runs.size(), -1),
        expr_pos(s.exprs.size(), -1);
    std::int64_t off = 0x48;
    const std::int64_t groups_off = off;
    off += 0x20 * static_cast<std::int64_t>(groups.size());
    for (const int st : states) state_pos[static_cast<std::size_t>(st)] = off, off += 0x48;
    for (const int c : conds) cond_pos[static_cast<std::size_t>(c)] = off, off += 0x38;
    std::int64_t nargs = 0, nptrs = 0;
    for (const int r : cmdruns) run_pos[static_cast<std::size_t>(r)] = off, off += 0x18 * static_cast<std::int64_t>(s.runs[r].items.size());
    for (const int r : argruns) {
        run_pos[static_cast<std::size_t>(r)] = off;
        off += 0x10 * static_cast<std::int64_t>(s.runs[r].items.size());
        nargs += static_cast<std::int64_t>(s.runs[r].items.size());
    }
    const std::int64_t condoffs = off;
    for (const int r : condruns) {
        run_pos[static_cast<std::size_t>(r)] = off;
        off += 8 * static_cast<std::int64_t>(s.runs[r].items.size());
        nptrs += static_cast<std::int64_t>(s.runs[r].items.size());
    }
    for (const int x : exprs) expr_pos[static_cast<std::size_t>(x)] = off, off += static_cast<std::int64_t>(s.exprs[x].code.size());
    const std::int64_t name_block = off, name_off = off + (off & 1);
    const std::int64_t size = name_off + static_cast<std::int64_t>(s.name.size());

    std::vector<std::uint8_t> out(kBase + static_cast<std::size_t>(size), 0);
    auto P = [&](std::int64_t rel, std::int64_t v) { put64(out, kBase + static_cast<std::size_t>(rel), v); };
    auto ref = [&](int r) -> std::pair<std::int64_t, std::int64_t> {
        const Run& run = s.runs[static_cast<std::size_t>(r)];
        if (run.items.empty()) return {run.empty_off, 0};
        return {run_pos[static_cast<std::size_t>(r)], static_cast<std::int64_t>(run.items.size())};
    };
    std::copy(s.dhead.begin(), s.dhead.end(), out.begin() + kBase);
    P(0x18, groups.empty() ? s.empty_groups_off : groups_off);
    P(0x20, static_cast<std::int64_t>(groups.size()));
    P(0x28, name_off);
    P(0x30, static_cast<std::int64_t>(s.name.size() / 2));
    std::copy(s.dtail.begin(), s.dtail.end(), out.begin() + kBase + 0x38);
    for (std::size_t k = 0; k < groups.size(); ++k) {
        const Group& g = s.groups[static_cast<std::size_t>(groups[k])];
        const std::int64_t at = groups_off + 0x20 * static_cast<std::int64_t>(k), first = state_pos[static_cast<std::size_t>(g.states[0])];
        P(at, g.id), P(at + 8, first), P(at + 16, static_cast<std::int64_t>(g.states.size())), P(at + 24, first);
    }
    for (const int st : states) {
        const State& x = s.states[static_cast<std::size_t>(st)];
        const std::int64_t at = state_pos[static_cast<std::size_t>(st)];
        P(at, x.id);
        const int lists[4] = {x.conds, x.entry, x.exit, x.whiles};
        for (int k = 0; k < 4; ++k) {
            const auto [ro, rn] = ref(lists[k]);
            P(at + 8 + 16 * k, ro), P(at + 16 + 16 * k, rn);
        }
    }
    for (const int c : conds) {
        const Cond& x = s.conds[static_cast<std::size_t>(c)];
        const std::int64_t at = cond_pos[static_cast<std::size_t>(c)];
        P(at, x.target >= 0 ? state_pos[static_cast<std::size_t>(x.target)] : -1);
        const auto [po, pn] = ref(x.pass);
        const auto [so, sn] = ref(x.subs);
        P(at + 8, po), P(at + 16, pn), P(at + 24, so), P(at + 32, sn);
        P(at + 40, expr_pos[static_cast<std::size_t>(x.eval)]);
        P(at + 48, static_cast<std::int64_t>(s.exprs[static_cast<std::size_t>(x.eval)].code.size()));
    }
    for (const int r : cmdruns) {
        std::int64_t at = run_pos[static_cast<std::size_t>(r)];
        for (const int cm : s.runs[static_cast<std::size_t>(r)].items) {
            const Cmd& x = s.cmds[static_cast<std::size_t>(cm)];
            put32(out, kBase + static_cast<std::size_t>(at), x.bank);
            put32(out, kBase + static_cast<std::size_t>(at) + 4, x.id);
            const auto [ao, an] = ref(x.args);
            P(at + 8, ao), P(at + 16, an);
            at += 0x18;
        }
    }
    for (const int r : argruns) {
        std::int64_t at = run_pos[static_cast<std::size_t>(r)];
        for (const int x : s.runs[static_cast<std::size_t>(r)].items) {
            P(at, expr_pos[static_cast<std::size_t>(x)]), P(at + 8, static_cast<std::int64_t>(s.exprs[static_cast<std::size_t>(x)].code.size()));
            at += 0x10;
        }
    }
    for (const int r : condruns) {
        std::int64_t at = run_pos[static_cast<std::size_t>(r)];
        for (const int c : s.runs[static_cast<std::size_t>(r)].items) P(at, cond_pos[static_cast<std::size_t>(c)]), at += 8;
    }
    for (const int x : exprs)
        std::copy(s.exprs[x].code.begin(), s.exprs[x].code.end(), out.begin() + kBase + static_cast<std::size_t>(expr_pos[static_cast<std::size_t>(x)]));
    std::copy(s.name.begin(), s.name.end(), out.begin() + kBase + static_cast<std::size_t>(name_off));
    std::vector<std::uint8_t> h = s.head;
    put32(h, 0x14, size);
    put32(h, 0x28, static_cast<std::int64_t>(groups.size()));
    put32(h, 0x30, static_cast<std::int64_t>(states.size()));
    put32(h, 0x38, static_cast<std::int64_t>(conds.size()));
    put32(h, 0x40, static_cast<std::int64_t>(cmds.size()));
    put32(h, 0x48, nargs);
    put32(h, 0x4c, condoffs);
    put32(h, 0x50, nptrs);
    put32(h, 0x54, name_block);
    put32(h, 0x58, static_cast<std::int64_t>(s.name.size() / 2));
    put32(h, 0x5c, size);
    put32(h, 0x64, size);
    std::copy(h.begin(), h.end(), out.begin());
    return out;
}

int Script::expr(std::vector<std::uint8_t> code) {
    exprs.push_back({std::move(code), kNew});
    return static_cast<int>(exprs.size() - 1);
}

int Script::run(std::vector<int> items) {
    runs.push_back({std::move(items), kNew, -1});
    return static_cast<int>(runs.size() - 1);
}

int Script::cmd(std::int32_t bank, std::int32_t id, const std::vector<std::vector<std::uint8_t>>& args) {
    std::vector<int> xs;
    for (const auto& a : args) xs.push_back(expr(a));
    cmds.push_back({bank, id, run(std::move(xs))});
    return static_cast<int>(cmds.size() - 1);
}

int Script::cond(int target, std::vector<std::uint8_t> eval, int pass, int subs) {
    const int ev = expr(std::move(eval));
    const int p = pass >= 0 ? pass : run({});
    const int sb = subs >= 0 ? subs : run({});
    conds.push_back({target, ev, p, sb, kNew});
    return static_cast<int>(conds.size() - 1);
}

int Script::state(std::int64_t id) {
    const int c = run({}), e = run({}), x = run({}), w = run({});
    states.push_back({id, c, e, x, w});
    return static_cast<int>(states.size() - 1);
}

Group* Script::group(std::int64_t id) {
    for (Group& g : groups)
        if (g.id == id) return &g;
    return nullptr;
}

int Script::state_in(const Group& g, std::int64_t id) const {
    for (const int st : g.states)
        if (states[static_cast<std::size_t>(st)].id == id) return st;
    return -1;
}

std::vector<std::uint8_t> lit(std::int64_t v) {
    if (v >= -64 && v <= 63) return {static_cast<std::uint8_t>(0x40 + v)};
    std::vector<std::uint8_t> out{0x82, 0, 0, 0, 0};
    const auto w = static_cast<std::int32_t>(v);
    std::memcpy(out.data() + 1, &w, 4);
    return out;
}

std::vector<std::uint8_t> call(int fn, const std::vector<std::int64_t>& args) {
    std::vector<std::uint8_t> out = lit(fn);
    for (const std::int64_t a : args) {
        const auto l = lit(a);
        out.insert(out.end(), l.begin(), l.end());
    }
    out.push_back(static_cast<std::uint8_t>(0x84 + args.size()));
    out.push_back(0xa6);
    return out;
}

std::vector<std::uint8_t> done(std::vector<std::uint8_t> code) {
    code.push_back(0xa1);
    return code;
}

}  // namespace esd
