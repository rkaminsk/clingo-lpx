#include <solving.hh>
#include <parsing.hh>

#include <unordered_set>

struct Solver::Prepare {
    index_t add_non_basic(Solver &s, Clingo::Symbol var) {
        auto [jt, res] = s.indices_.emplace(var, n_vars);
        if (res) {
            s.variables_.emplace_back();
            // Note: that this makes it possible to use `Solver::non_basic_`
            // during initialization
            s.variables_[s.n_non_basic_].index = n_vars;
            s.variables_[n_vars].reserve_index = s.n_non_basic_;
            ++n_vars;
            ++s.n_non_basic_;
        }
        return s.variables_[jt->second].reserve_index;
    }

    index_t add_basic(Solver &s) {
        basic.emplace_back(n_vars);
        s.variables_.emplace_back();
        ++n_vars;
        return basic.size() - 1;
    }

    std::vector<std::pair<index_t, Number>> add_row(Solver &s, Inequality const &x) {
        std::vector<std::pair<index_t, Number>> row;
        std::vector<Clingo::Symbol> vars;
        std::unordered_map<Clingo::Symbol, Number> cos;

        // combine cofficients
        for (auto const &y : x.lhs) {
            if (y.co == 0) {
                continue;
            }
            if (auto [it, res] = cos.emplace(y.var, y.co); !res) {
                it->second += y.co;
                if (it->second == 0) {
                    cos.erase(it);
                }
            }
            else {
                vars.emplace_back(y.var);
            }
        }

        // add non-basic variables for the remaining non-zero coefficients
        for (auto &var : vars) {
            if (auto it = cos.find(var); it != cos.end()) {
                index_t j = add_non_basic(s, var);
                row.emplace_back(j, it->second);
            }
        }

        return row;
    }

    void finish(Solver &s) {
        s.n_basic_ = basic.size();
        int i = s.n_non_basic_;
        for (auto &index : basic) {
            s.variables_[index].reserve_index = i;
            s.variables_[i].index = index;
            ++i;
        }
    }

    index_t n_vars{0};
    std::vector<index_t> basic;
};

bool Solver::Variable::update_upper(Solver &s, Clingo::Assignment ass, Bound const &bound) {
    if (!has_upper() || bound.value < upper()) {
        if (!has_upper() || ass.level(upper_bound->lit) < ass.decision_level()) {
            s.bound_trail_.emplace_back(bound.variable, Relation::LessEqual, upper_bound);
        }
        upper_bound = &bound;
    }
    return !has_lower() || lower() <= upper();
}

bool Solver::Variable::update_lower(Solver &s, Clingo::Assignment ass, Bound const &bound) {
    if (!has_lower() || bound.value > lower()) {
        if (!has_lower() || ass.level(lower_bound->lit) < ass.decision_level()) {
            if (upper_bound != &bound) {
                s.bound_trail_.emplace_back(bound.variable, Relation::GreaterEqual, lower_bound);
            }
            else {
                // Note: this assumes that update_lower is called right after update_upper for the same bound
                std::get<1>(s.bound_trail_.back()) = Relation::Equal;
            }
        }
        lower_bound = &bound;
    }
    return !has_upper() || lower() <= upper();
}

bool Solver::Variable::update(Solver &s, Clingo::Assignment ass, Bound const &bound) {
    switch (bound.rel) {
        case Relation::LessEqual: {
            return update_upper(s, ass, bound);
        }
        case Relation::GreaterEqual: {
            return update_lower(s, ass, bound);
        }
        case Relation::Equal: {
            break;
        }
    }
    return update_upper(s, ass, bound) && update_lower(s, ass, bound);
}

void Solver::Variable::set_value(Solver &s, index_t lvl, Number const &val, bool add) {
    // We can always assume that the assignment on a previous level was satisfying.
    // Thus, we simply store the old values to be able to restore them when backtracking.
    if (lvl != level) {
        s.assignment_trail_.emplace_back(level, this - s.variables_.data(), value);
        level = lvl;
    }
    if (add) {
        value += val;
    }
    else {
        value = val;
    }
}

bool Solver::Variable::has_conflict() const {
    return (has_lower() && value < lower()) || (has_upper() && value > upper());
}

void Statistics::reset() {
    *this = {};
}

Solver::Variable &Solver::basic_(index_t i) {
    assert(i < n_basic_);
    return variables_[variables_[i + n_non_basic_].index];
}

Solver::Variable &Solver::non_basic_(index_t j) {
    assert(j < n_non_basic_);
    return variables_[variables_[j].index];
}

void Solver::enqueue_(index_t i) {
    assert(i < n_basic_);
    auto ii = variables_[i + n_non_basic_].index;
    auto &xi = variables_[ii];
    if (!xi.queued && xi.has_conflict()) {
        conflicts_.emplace(ii);
        xi.queued = true;
    }
}

bool Solver::prepare(Clingo::PropagateInit &init, std::vector<Inequality> &&inequalities) {
    tableau_.clear();
    variables_.clear();
    indices_.clear();
    statistics_.reset();
    n_basic_ = 0;
    n_non_basic_ = 0;

    inequalities_ = std::move(inequalities);
    for (auto &x : inequalities_) {
        x.lit = init.solver_literal(x.lit);
        init.add_watch(x.lit);
    }

    // TODO: Bounds associated with a variable form a propagation chain. We can
    // add binary clauses to propagate them. For example
    //
    //     `x >= u` implies not `x <= l` for all `l < u`.
    //
    // Care has to be taken because we cannot use
    //
    //     `x >= u` implies `x >= u'` for all u' >= u
    //
    // because I am going for a non-strict defined semantics.

    auto ass = init.assignment();

    Prepare prep;
    for (auto const &x : inequalities_) {
        if (ass.is_false(x.lit)) {
            continue;
        }

        // transform inequality into row suitable for tableau
        auto row = prep.add_row(*this, x);

        // check bound against 0
        if (row.empty()) {
            switch (x.rel) {
                case Relation::LessEqual: {
                    if (x.rhs > 0 && !init.add_clause({-x.lit})) {
                        return false;
                    }
                    break;
                }
                case Relation::GreaterEqual: {
                    if (x.rhs < 0 && !init.add_clause({-x.lit})) {
                        return false;
                    }
                    break;
                }
                case Relation::Equal: {
                    if (x.rhs != 0 && !init.add_clause({-x.lit})) {
                        return false;
                    }
                    break;
                }
            }
        }
        // add a bound to a non-basic variable
        else if (row.size() == 1) {
            auto const &[j, v] = row.front();
            auto &xj = non_basic_(j);
            bounds_.emplace(x.lit, Bound{
                x.rhs / v,
                variables_[j].index,
                x.lit,
                v < 0 ? invert(x.rel) : x.rel});
        }
        // add an inequality
        else {
            auto i = prep.add_basic(*this);
            bounds_.emplace(x.lit, Bound{
                x.rhs,
                static_cast<index_t>(variables_.size() - 1),
                x.lit,
                x.rel});
            for (auto const &[j, v] : row) {
                tableau_.set(i, j, v);
            }
        }
    }

    prep.finish(*this);

    for (size_t i = 0; i < n_basic_; ++i) {
        enqueue_(i);
    }

    assert_extra(check_tableau_());
    assert_extra(check_basic_());
    assert_extra(check_non_basic_());

    return true;
}

std::vector<std::pair<Clingo::Symbol, Number>> Solver::assignment() const {
    std::vector<std::pair<Clingo::Symbol, Number>> ret;
    index_t k{0};
    for (auto var : vars_()) {
        if (auto it = indices_.find(var); it != indices_.end()) {
            ret.emplace_back(var, variables_[it->second].value);
        }
        else {
            ret.emplace_back(var, 0);
        }
    }
    return ret;
}

bool Solver::solve(Clingo::PropagateControl &ctl, Clingo::LiteralSpan lits) {
    index_t i{0};
    index_t j{0};
    Number const *v{nullptr};

    auto ass = ctl.assignment();
    auto level = ass.decision_level();

    if (trail_offset_.empty() || trail_offset_.back().level < level) {
        trail_offset_.emplace_back(TrailOffset{
            ass.decision_level(),
            static_cast<index_t>(bound_trail_.size()),
            static_cast<index_t>(assignment_trail_.size())});
    }

    for (auto lit : lits) {
        for (auto it = bounds_.find(lit), ie = bounds_.end(); it != ie && it->first == lit; ++it) {
            auto const &[lit, bound] = *it;
            auto &x = variables_[bound.variable];
            if (!x.update(*this, ctl.assignment(), bound)) {
                conflict_clause_.clear();
                conflict_clause_.emplace_back(-x.upper_bound->lit);
                conflict_clause_.emplace_back(-x.lower_bound->lit);
                return false;
            }
            if (x.reserve_index < n_non_basic_) {
                if (x.has_lower() && x.value < x.lower()) {
                    update_(level, x.reserve_index, x.lower());
                }
                else if (x.has_upper() && x.value > x.upper()) {
                    update_(level, x.reserve_index, x.upper());
                }
            }
            else {
                enqueue_(x.reserve_index - n_non_basic_);
            }
        }
    }

    assert_extra(check_tableau_());
    assert_extra(check_basic_());
    assert_extra(check_non_basic_());

    while (true) {
        switch (select_(i, j, v)) {
            case State::Satisfiable: {
#ifdef CLINGOLP_KEEP_SAT_ASSIGNMENT
                for (auto &[level, index, number] : assignment_trail_) {
                    variables_[index].level = 0;
                }
                for (auto it = trail_offset_.rbegin(), ie = trail_offset_.rend(); it != ie; ++it) {
                    if (it->assignment > 0) {
                        it->assignment = 0;
                    }
                    else {
                        break;
                    }
                }
                assignment_trail_.clear();
#endif
                return true;
            }
            case State::Unsatisfiable: {
                return false;
            }
            case State::Unknown: {
                assert(v != nullptr);
                pivot_(level, i, j, *v);
            }
        }
    }
}

void Solver::undo() {
    // this function restores the last satisfying assignment
    auto &offset = trail_offset_.back();

    // undo bound updates
    for (auto it = bound_trail_.begin() + offset.bound, ie = bound_trail_.end(); it != ie; ++it) {
        auto [var, rel, bound] = *it;
        switch (rel) {
            case Relation::LessEqual: {
                variables_[var].upper_bound = bound;
                break;
            }
            case Relation::GreaterEqual: {
                variables_[var].lower_bound = bound;
                break;
            }
            case Relation::Equal: {
                variables_[var].upper_bound = bound;
                variables_[var].lower_bound = bound;
                break;
            }
        }
    }
    bound_trail_.resize(offset.bound);

    // undo assignments
    for (auto it = assignment_trail_.begin() + offset.assignment, ie = assignment_trail_.end(); it != ie; ++it) {
        auto &[level, index, number] = *it;
        variables_[index].level = level;
        variables_[index].value.swap(number);
    }
    assignment_trail_.resize(offset.assignment);

    // empty queue
    for (; !conflicts_.empty(); conflicts_.pop()) {
        variables_[conflicts_.top()].queued = false;
    }

    trail_offset_.pop_back();

    assert_extra(check_solution_());
}

Statistics const &Solver::statistics() const {
    return statistics_;
}

std::vector<Clingo::Symbol> Solver::vars_() const {
    std::unordered_set<Clingo::Symbol> var_set;
    for (auto const &x : inequalities_) {
        for (auto const &y : x.lhs) {
            if (y.var.type() != Clingo::SymbolType::Number) {
                var_set.emplace(y.var);
            }
        }
    }
    std::vector<Clingo::Symbol> var_vec{var_set.begin(), var_set.end()};
    std::sort(var_vec.begin(), var_vec.end());
    return var_vec;
};

bool Solver::check_tableau_() {
    for (index_t i{0}; i < n_basic_; ++i) {
        Number v_i{0};
        tableau_.update_row(i, [&](index_t j, Number const &a_ij){
            v_i += non_basic_(j).value * a_ij;
        });
        if (v_i != basic_(i).value) {
            return false;
        }
    }
    return true;
}

bool Solver::check_basic_() {
    for (index_t i = 0; i < n_basic_; ++i) {
        auto &xi = basic_(i);
        if (xi.has_lower() && xi.value < xi.lower() && !xi.queued) {
            return false;
        }
        if (xi.has_upper() && xi.value > xi.upper() && !xi.queued) {
            return false;
        }
    }
    return true;
}

bool Solver::check_non_basic_() {
    for (index_t j = 0; j < n_non_basic_; ++j) {
        auto &xj = non_basic_(j);
        if (xj.has_lower() && xj.value < xj.lower()) {
            return false;
        }
        if (xj.has_upper() && xj.value > xj.upper()) {
            return false;
        }
    }
    return true;
}

bool Solver::check_solution_() {
    for (auto &x : variables_) {
        if (x.has_lower() && x.lower() > x.value) {
            return false;
        }
        if (x.has_upper() && x.value > x.upper()) {
            return false;
        }
    }
    return check_tableau_() && check_basic_();
}

void Solver::update_(index_t level, index_t j, Number v) {
    auto &xj = non_basic_(j);
    tableau_.update_col(j, [&](index_t i, Number const &a_ij) {
        basic_(i).set_value(*this, level, a_ij * (v - xj.value), true);
        enqueue_(i);
    });
    xj.set_value(*this, level, v, false);
}

void Solver::pivot_(index_t level, index_t i, index_t j, Number const &v) {
    auto &a_ij = tableau_.unsafe_get(i, j);
    assert(a_ij != 0);

    auto &xi = basic_(i);
    auto &xj = non_basic_(j);

    // adjust assignment
    Number dj = (v - xi.value) / a_ij;
    xi.set_value(*this, level, v, false);
    xj.set_value(*this, level, dj, true);
    tableau_.update_col(j, [&](index_t k, Number const &a_kj) {
        if (k != i) {
            basic_(k).set_value(*this, level, a_kj * dj, true);
            enqueue_(k);
        }
    });
    assert_extra(check_tableau_());

    // swap variables x_i and x_j
    std::swap(xi.reserve_index, xj.reserve_index);
    std::swap(variables_[i + n_non_basic_].index, variables_[j].index);
    enqueue_(i);

    // invert row i
    tableau_.update_row(i, [&](index_t k, Number &a_ik) {
        if (k != j) {
            a_ik /= -a_ij;
        }
    });
    a_ij = 1 / a_ij;

    // eliminate x_j from rows k != i
    tableau_.update_col(j, [&](index_t k, Number &a_kj) {
        if (k != i) {
            // Note that this call does not invalidate active iterators:
            // - row i is unaffected because k != i
            // - there are no insertions in column j because each a_kj != 0
            tableau_.update_row(i, [&](index_t l, Number const &a_il) {
                if (l != j) {
                    tableau_.update(k, l, [&](Number &a_kl) { a_kl += a_il * a_kj; });
                }
            });
            // Note that a_ij was inverted above.
            a_kj *= a_ij;
        }
    });

    ++statistics_.pivots_;
    assert_extra(check_tableau_());
    assert_extra(check_basic_());
    assert_extra(check_non_basic_());
}

bool Solver::select_(bool upper, Variable &x) {
    if (upper) {
        if (!x.has_upper() || x.value < x.upper()) {
            return true;
        }
        conflict_clause_.emplace_back(-x.upper_bound->lit);
    }
    else {
        if (!x.has_lower() || x.value > x.lower()) {
            return true;
        }
        conflict_clause_.emplace_back(-x.lower_bound->lit);
    }
    return false;
}

Solver::State Solver::select_(index_t &ret_i, index_t &ret_j, Number const *&ret_v) {
    // This implements Bland's rule selecting the variables with the smallest
    // indices for pivoting.

    for (; !conflicts_.empty(); conflicts_.pop()) {
        auto ii = conflicts_.top();
        auto &xi = variables_[ii];
        auto i = xi.reserve_index;
        assert(ii == variables_[i].index);
        xi.queued = false;
        // the queue might contain variables that meanwhile became basic
        if (i < n_non_basic_) {
            continue;
        }
        i -= n_non_basic_;

        if (xi.has_lower() && xi.value < xi.lower()) {
            conflict_clause_.clear();
            conflict_clause_.emplace_back(-xi.lower_bound->lit);
            index_t kk = variables_.size();
            tableau_.update_row(i, [&](index_t j, Number const &a_ij) {
                auto jj = variables_[j].index;
                if (jj < kk && select_(a_ij > 0, variables_[jj])) {
                    kk = jj;
                    ret_i = i;
                    ret_j = j;
                    ret_v = &xi.lower();
                }
            });
            if (kk == variables_.size()) {
                return State::Unsatisfiable;
            }
            return State::Unknown;
        }

        if (xi.has_upper() && xi.value > xi.upper()) {
            conflict_clause_.clear();
            conflict_clause_.emplace_back(-xi.upper_bound->lit);
            index_t kk = variables_.size();
            tableau_.update_row(i, [&](index_t j, Number const &a_ij) {
                auto jj = variables_[j].index;
                if (jj < kk && select_(a_ij < 0, variables_[jj])) {
                    kk = jj;
                    ret_i = i;
                    ret_j = j;
                    ret_v = &xi.upper();
                }
            });
            if (kk == variables_.size()) {
                return State::Unsatisfiable;
            }
            return State::Unknown;
        }
    }

    assert(check_solution_());

    return State::Satisfiable;
}

void ClingoLPPropagator::init(Clingo::PropagateInit &init) {
    slvs_.reserve(init.number_of_threads());
    for (size_t i = 0, e = init.number_of_threads(); i != e; ++i) {
        slvs_.emplace_back();
        if (!slvs_.back().prepare(init, evaluate_theory(init.theory_atoms()))) {
            return;
        }
    }
}

void ClingoLPPropagator::on_statistics(Clingo::UserStatistics step, Clingo::UserStatistics accu) {
    auto step_simplex = step.add_subkey("Simplex", Clingo::StatisticsType::Map);
    auto step_pivots = step_simplex.add_subkey("Pivots", Clingo::StatisticsType::Value);
    auto accu_simplex = accu.add_subkey("Simplex", Clingo::StatisticsType::Map);
    auto accu_pivots = accu_simplex.add_subkey("Pivots", Clingo::StatisticsType::Value);
    for (auto const &slv : slvs_) {
        step_pivots.set_value(slv.statistics().pivots_);
        accu_pivots.set_value(accu_pivots.value() + slv.statistics().pivots_);
    }
}

void ClingoLPPropagator::propagate(Clingo::PropagateControl &ctl, Clingo::LiteralSpan changes) {
    auto &slv = slvs_[ctl.thread_id()];
    if (!slv.solve(ctl, changes)) {
        ctl.add_clause(slv.reason());
    }
}

void ClingoLPPropagator::undo(Clingo::PropagateControl const &ctl, Clingo::LiteralSpan changes) noexcept {
    slvs_[ctl.thread_id()].undo();
}
