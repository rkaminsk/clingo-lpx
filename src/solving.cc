#include <solving.hh>

#include <unordered_set>

void Statistics::reset() {
    *this = {};
}

Solver::Solver(std::vector<Equation> &&equations)
: equations_{std::move(equations)} { }

void Solver::prepare() {
    assignment_.clear();
    bounds_.clear();
    tableau_.clear();
    variables_.clear();
    indices_.clear();
    statistics_.reset();

    index_t i{0};
    index_t n_vars{0};
    std::vector<index_t> basic;
    std::vector<index_t> non_basic;
    for (auto const &x : equations_) {
        // combine coefficients in equation
        std::vector<Clingo::Symbol> vars;
        std::unordered_map<Clingo::Symbol, Number> cos;
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
        // add the equation
        // TODO: if cos.size() == 1 we do not have to add the equation
        // but can add a bound for the variable and updating its value to fullfill the bound
        for (auto &var : vars) {
            if (auto it = cos.find(var); it != cos.end()) {
                auto [jt, res] = indices_.emplace(var, n_vars);
                if (res) {
                    non_basic.emplace_back(n_vars);
                    bounds_.emplace_back();
                    assignment_.emplace_back();
                    ++n_vars;
                }
                index_t j = std::distance(non_basic.begin(), std::lower_bound(non_basic.begin(), non_basic.end(), jt->second));
                tableau_.set(i, j, it->second);
            }
        }

        // add a basic variable for the equation
        basic.emplace_back(n_vars);
        bounds_.emplace_back();
        assignment_.emplace_back();
        ++n_vars;
        ++i;
        switch (x.op) {
            case Operator::LessEqual: {
                bounds_.back().upper = x.rhs;
                break;
            }
            case Operator::GreaterEqual: {
                bounds_.back().lower = x.rhs;
                break;
            }
            case Operator::Equal: {
                bounds_.back().lower = x.rhs;
                bounds_.back().upper = x.rhs;
                break;
            }
        }
    }

    n_non_basic_ = non_basic.size();
    for (auto &var : non_basic) {
        variables_.emplace_back(var);
    }
    n_basic_ = basic.size();
    for (auto &var : basic) {
        variables_.emplace_back(var);
    }
    assert_extra(check_tableau_());
    assert_extra(check_non_basic_());
}

std::optional<std::vector<std::pair<Clingo::Symbol, Number>>> Solver::solve() {
    index_t i{0};
    index_t j{0};
    Number v{0};

    while (true) {
        switch (select_(i, j, v)) {
            case State::Satisfiable: {
                std::vector<std::pair<Clingo::Symbol, Number>> ret;
                index_t k{0};
                for (auto var : vars_()) {
                    if (auto it = indices_.find(var); it != indices_.end()) {
                        ret.emplace_back(var, assignment_[it->second]);
                    }
                    else {
                        ret.emplace_back(var, 0);
                    }
                }
                return ret;
            }
            case State::Unsatisfiable: {
                return std::nullopt;
            }
            case State::Unknown: {
                pivot_(i, j, v);
            }
        }
    }
}

Statistics const &Solver::statistics() const {
    return statistics_;
}

std::vector<Clingo::Symbol> Solver::vars_() {
    std::unordered_set<Clingo::Symbol> var_set;
    for (auto const &x : equations_) {
        for (auto const &y : x.lhs) {
            var_set.emplace(y.var);
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
            v_i += assignment_[variables_[j]] * a_ij;
        });
        if (v_i != assignment_[variables_[n_non_basic_ + i]]) {
            return false;
        }
    }
    return true;
}

bool Solver::check_non_basic_() {
    for (index_t j = 0; j < n_non_basic_; ++j) {
        auto xj = variables_[j];
        auto const &[lower, upper] = bounds_[xj];
        if (lower && assignment_[xj] < *lower) {
            return false;
        }
        if (upper && assignment_[xj] > *upper) {
            return false;
        }
    }
    return true;
}

void Solver::pivot_(index_t i, index_t j, Number const &v) {
    auto a_ij = tableau_.get(i, j);
    assert(a_ij != 0);

    // adjust assignment
    auto ii = i + n_non_basic_;
    Number dj = (v - assignment_[variables_[ii]]) / a_ij;
    assignment_[variables_[ii]] = v;
    assignment_[variables_[j]] += dj;
    tableau_.update_col(j, [&](index_t k, Number const &a_kj) {
        if (k != i) {
            // Note that a bound can become conflicting here
            assignment_[variables_[n_non_basic_ + k]] += a_kj * dj;
        }
    });
    assert_extra(check_tableau_());

    // swap variables x_i and x_j
    std::swap(variables_[ii], variables_[j]);

    // invert row i
    tableau_.update_row(i, [&](index_t k, Number &a_ik) {
        if (k == j) {
            a_ik = 1 / a_ij;
        }
        else {
            a_ik /= -a_ij;
        }
    });

    // eliminate x_j from rows k != i
    tableau_.update_col(j, [&](index_t k, Number const &a_kj) {
        if (k != i) {
            tableau_.update_row(i, [&](index_t l, Number const &a_il) {
                Number a_kl;
                if (l == j) {
                    a_kl = a_kj / a_ij;
                }
                else {
                    a_kl = tableau_.get(k, l) + a_il * a_kj;
                }
                // Note that this call does not invalidate active iterators:
                // - row i is unaffected because k != i
                // - there are no insertions in column j because each a_kj != 0
                //   (values in the column can change though)
                tableau_.set(k, l, a_kl);
            });
        }
    });

    ++statistics_.pivots_;
    assert_extra(check_tableau_());
    assert_extra(check_non_basic_());
}

Solver::State Solver::select_(index_t &ret_i, index_t &ret_j, Number &ret_v) {
    // TODO: This can be done while pivoting as well!
    std::vector<std::pair<index_t, index_t>> basic;
    std::vector<std::pair<index_t, index_t>> non_basic;
    for (index_t i = 0; i < n_basic_; ++i) {
        basic.emplace_back(i, variables_[i + n_non_basic_]);
    }
    std::sort(basic.begin(), basic.end(), [](auto const &a, auto const &b){ return a.second < b.second; });
    for (index_t j = 0; j < n_non_basic_; ++j) {
        non_basic.emplace_back(j, variables_[j]);
    }
    std::sort(non_basic.begin(), non_basic.end(), [](auto const &a, auto const &b){ return a.second < b.second; });

    for (auto [i, xi] : basic) {
        auto const &axi = assignment_[xi];

        if (auto const &li = bounds_[xi].lower; li && axi < *li) {
            for (auto [j, xj] : non_basic) {
                auto const &a_ij = tableau_.get(i, j);
                auto const &v_xj = assignment_[xj];
                if ((a_ij > 0 && (xj < n_non_basic_ || !bounds_[xj].upper || v_xj < *bounds_[xj].upper)) ||
                    (a_ij < 0 && (xj < n_non_basic_ || !bounds_[xj].lower || v_xj > *bounds_[xj].lower))) {
                    ret_i = i;
                    ret_j = j;
                    ret_v = *li;
                    return State::Unknown;
                }
            }
            return State::Unsatisfiable;
        }

        if (auto const &ui = bounds_[xi].upper; ui && axi > *ui) {
            for (auto [j, xj] : non_basic) {
                auto const &a_ij = tableau_.get(i, j);
                auto const &v_xj = assignment_[xj];
                if ((a_ij < 0 && (xj < n_non_basic_ || !bounds_[xj].upper || v_xj < *bounds_[xj].upper)) ||
                    (a_ij > 0 && (xj < n_non_basic_ || !bounds_[xj].lower || v_xj > *bounds_[xj].lower))) {
                    ret_i = i;
                    ret_j = j;
                    ret_v = *ui;
                    return State::Unknown;
                }
            }
            return State::Unsatisfiable;
        }
    }

    return State::Satisfiable;
}