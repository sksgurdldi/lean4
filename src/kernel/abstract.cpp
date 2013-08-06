/*
Copyright (c) 2013 Microsoft Corporation. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.

Author: Leonardo de Moura
*/
#include <algorithm>
#include "abstract.h"
#include "free_vars.h"
#include "replace.h"

namespace lean {
expr abstract(expr const & e, unsigned n, expr const * s) {
    lean_assert(std::all_of(s, s+n, closed));

    auto f = [=](expr const & e, unsigned offset) -> expr {
        unsigned i = n;
        while (i > 0) {
            --i;
            if (s[i] == e)
                return mk_var(offset + n - i - 1);
        }
        return e;
    };

    return replace_fn<decltype(f)>(f)(e);
}
expr abstract_p(expr const & e, unsigned n, expr const * s) {
    lean_assert(std::all_of(s, s+n, closed));

    auto f = [=](expr const & e, unsigned offset) -> expr {
        unsigned i = n;
        while (i > 0) {
            --i;
            if (is_eqp(s[i], e))
                return mk_var(offset + n - i - 1);
        }
        return e;
    };

    return replace_fn<decltype(f)>(f)(e);
}
expr Fun(std::initializer_list<std::pair<expr const &, expr const &>> const & l, expr const & b) {
    expr r = b;
    auto it = l.end();
    while (it != l.begin()) {
        --it;
        auto const & p = *it;
        r = Fun(p.first, p.second, r);
    }
    return r;
}
expr Pi(std::initializer_list<std::pair<expr const &, expr const &>> const & l, expr const & b) {
    expr r = b;
    auto it = l.end();
    while (it != l.begin()) {
        --it;
        auto const & p = *it;
        r = Pi(p.first, p.second, r);
    }
    return r;
}
}
