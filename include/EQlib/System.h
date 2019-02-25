#pragma once

#include "Define.h"
#include "Dof.h"
#include "Element.h"
#include "Solver/SolverLDLT.h"
#include "Solver/SolverLSMR.h"

#include <fmt/format.h>

#include <tbb/pipeline.h>

#include <set>
#include <unordered_map>
#include <unordered_set>

namespace EQlib {

class System
{
private:    // types
    struct Index
    {
        int local;
        int global;

        bool operator<(const Index& other) const noexcept
        {
            return this->global < other.global;
        }
    };

private:    // variables
    std::vector<Dof> m_dofs;

    std::unordered_map<Dof, int> m_dof_indices;

    int m_nb_free_dofs;
    int m_nb_fixed_dofs;

    std::vector<std::shared_ptr<Element>> m_elements;
    std::vector<std::vector<Index>> m_index_table;

    std::vector<std::set<int>> m_pattern;
    Eigen::VectorXi m_col_nonzeros;

    Sparse m_lhs;
    Vector m_rhs;
    Vector m_x;
    Vector m_target;
    Vector m_residual;

    int m_stopping_reason;

    std::unique_ptr<Solver> m_solver;

public:     // constructor
    System(
        std::vector<std::shared_ptr<Element>> elements,
        py::dict options)
    {
        // options

        const std::string linear_solver = get_or_default<std::string>(options,
            "linear_solver", "ldlt");

        // get dofs

        const auto nb_elements = elements.size();

        std::vector<std::vector<Dof>> element_dofs(nb_elements);

        for (size_t i = 0; i < nb_elements; i++) {
            const auto& element = elements[i];

            element_dofs[i] = element->dofs();
        }

        // create set of unique dofs

        std::unordered_set<Dof> dof_set;
        std::vector<Dof> free_dofs;
        std::vector<Dof> fixed_dofs;

        for (size_t i = 0; i < nb_elements; i++) {
            const auto& element = elements[i];

            for (const auto& dof : element_dofs[i]) {
                if (dof_set.find(dof) != dof_set.end()) {
                    continue;
                }

                dof_set.insert(dof);

                if (dof.isfixed()) {
                    fixed_dofs.push_back(dof);
                } else {
                    free_dofs.push_back(dof);
                }
            }
        }

        // store system size

        m_nb_free_dofs = static_cast<int>(free_dofs.size());
        m_nb_fixed_dofs = static_cast<int>(fixed_dofs.size());

        // create a vector of unique dofs

        m_dofs.reserve(m_nb_free_dofs + m_nb_fixed_dofs);

        m_dofs.insert(m_dofs.end(), free_dofs.begin(), free_dofs.end());
        m_dofs.insert(m_dofs.end(), fixed_dofs.begin(), fixed_dofs.end());

        // create a {dof -> index} map

        for (int i = 0; i < m_dofs.size(); i++) {
            m_dof_indices[m_dofs[i]] = i;
        }

        // create index table

        for (size_t i = 0; i < nb_elements; i++) {
            const auto& element = elements[i];

            const auto& dofs = element_dofs[i];

            const auto nb_dofs = dofs.size();

            std::vector<Index> dof_indices(nb_dofs);

            for (int local = 0; local < nb_dofs; local++) {
                const auto dof = dofs[local];
                const auto global = m_dof_indices[dof];
                dof_indices[local] = {local, global};
            }

            std::sort(dof_indices.begin(), dof_indices.end());

            m_index_table.push_back(dof_indices);
        }

        // analyze pattern

        std::vector<std::set<int>> pattern(m_nb_free_dofs);

        for (const auto& dof_indices : m_index_table) {
            const size_t nb_dofs = dof_indices.size();

            for (size_t row = 0; row < nb_dofs; row++) {
                const auto row_index = dof_indices[row];

                if (row_index.global >= nb_free_dofs()) {
                    continue;
                }

                for (size_t col = row; col < nb_dofs; col++) {
                    const auto col_index = dof_indices[col];

                    if (col_index.global >= nb_free_dofs()) {
                        continue;
                    }

                    pattern[col_index.global].insert(row_index.global);
                }
            }
        }

        m_col_nonzeros = Eigen::VectorXi(m_nb_free_dofs);

        for (int i = 0; i < pattern.size(); i++) {
            m_col_nonzeros[i] = static_cast<int>(pattern[i].size());
        }

        // store data

        m_pattern = std::move(pattern);

        m_elements = std::move(elements);

        // setup system vectors and matrices

        m_lhs = Sparse(nb_free_dofs(), nb_free_dofs());

        if (nb_free_dofs() > 0) {
            m_lhs.reserve(m_col_nonzeros);

            for (int col = 0; col < m_pattern.size(); col++) {
                for (const int row : m_pattern[col]) {
                    m_lhs.insert(row, col);
                }
            }
        }

        m_rhs = Vector(nb_free_dofs());

        m_x = Vector(nb_free_dofs());

        m_target = Vector(nb_free_dofs());
        m_residual = Vector(nb_free_dofs());

        // setup solver

        if (linear_solver == "ldlt") {
            m_solver = std::make_unique<SolverLDLT>();
        } else if (linear_solver == "lsmr") {
            m_solver = std::make_unique<SolverLSMR>(m_lhs);
        } else {
            std::cout << "error" << std::endl; // FIXME: throw exception
        }

        m_solver->analyze_pattern(m_lhs);
    }

public:     // getters and setters
    const std::vector<Dof>& dofs() const
    {
        return m_dofs;
    }

    int nb_dofs() const
    {
        return static_cast<int>(m_dofs.size());
    }

    int nb_free_dofs() const
    {
        return m_nb_free_dofs;
    }

    int nb_fixed_dofs() const
    {
        return m_nb_fixed_dofs;
    }

    Sparse lhs() const
    {
        return m_lhs;
    }

    Vector rhs() const
    {
        return m_rhs;
    }

    std::string stopping_reason_message() const
    {
        switch (m_stopping_reason) {
        case -1:
            return "Not solved";
        case 0:
            return "A solution was found, given rtol";
        case 1:
            return "A solution was found, given xtol";
        case 2:
            return "The iteration limit was reached";
        default:
            return "Error. Unknown stopping reason";
        }
    }

public:     // methods
    int dof_index(const Dof& dof) const
    {
        return m_dof_indices.at(dof);
    }

    void compute(py::dict options)
    {
        // options

        // ...

        // set lhs and rhs to zero

        for (int i = 0; i < m_lhs.outerSize(); i++) {
            for (Sparse::InnerIterator it(m_lhs, i); it; ++it){
                it.valueRef() = 0;
            }
        }

        m_rhs.setZero();

        // compute and add local lhs and rhs

        for (size_t i = 0; i < m_elements.size(); i++) {
            const auto& element = m_elements[i];

            const auto [local_lhs, local_rhs] = element->compute(options);

            const auto& dof_indices = m_index_table[i];

            const size_t nb_dofs = dof_indices.size();

            for (size_t row = 0; row < nb_dofs; row++) {
                const auto row_index = dof_indices[row];

                if (row_index.global >= nb_free_dofs()) {
                    continue;
                }

                m_rhs(row_index.global) += local_rhs(row_index.local);

                for (size_t col = row; col < nb_dofs; col++) {
                    const auto col_index = dof_indices[col];

                    if (col_index.global >= nb_free_dofs()) {
                        continue;
                    }

                    m_lhs.coeffRef(row_index.global, col_index.global) +=
                        local_lhs(row_index.local, col_index.local);
                }
            }
        }
    }

    void compute_parallel(py::dict options)
    {
        // options

        // ...

        // run parallel

        py::gil_scoped_release release;

        size_t index = 0;

        tbb::parallel_pipeline(8,
            tbb::make_filter<void, size_t>(
                tbb::filter::serial,
                [&](tbb::flow_control& fc) -> size_t {
                    if (index < m_elements.size()) {
                        return index++;
                    } else {
                        fc.stop();
                        return index;
                    }
                }
            ) &
            tbb::make_filter<size_t, std::tuple<size_t, std::pair<Matrix, Vector>>>(
                tbb::filter::parallel,
                [&](size_t i) -> std::tuple<size_t, std::pair<Matrix, Vector>> {
                    const auto& element = m_elements[i];

                    const auto local = element->compute(py::dict(options));

                    return {i, local};
                }
            ) &
            tbb::make_filter<std::tuple<size_t, std::pair<Matrix, Vector>>, void>(
                tbb::filter::serial_out_of_order,
                [&](std::tuple<size_t, std::pair<Matrix, Vector>> results) {
                    const auto i = std::get<0>(results);
                    const auto& [local_lhs, local_rhs] = std::get<1>(results);

                    const auto& dof_indices = m_index_table[i];

                    const size_t nb_dofs = dof_indices.size();

                    for (size_t row = 0; row < nb_dofs; row++) {
                        const auto row_index = dof_indices[row];

                        if (row_index.global >= nb_free_dofs()) {
                            continue;
                        }

                        m_rhs(row_index.global) += local_rhs(row_index.local);

                        for (size_t col = row; col < nb_dofs; col++) {
                            const auto col_index = dof_indices[col];

                            if (col_index.global >= nb_free_dofs()) {
                                continue;
                            }

                            m_lhs.coeffRef(row_index.global, col_index.global)
                                += local_lhs(row_index.local, col_index.local);
                        }
                    }
                }
            )
        );
    }

    void solve(py::dict options)
    {
        // options

        const double lambda = get_or_default(options, "lambda", 1.0);
        const int maxiter = get_or_default(options, "maxiter", 100);
        const double rtol = get_or_default(options, "rtol", 1e-7);
        const double xtol = get_or_default(options, "xtol", 1e-7);

        // setup

        for (int i = 0; i < nb_free_dofs(); i++) {
            m_target[i] = m_dofs[i].target();
        }

        if (lambda != 1.0) {
            m_target *= lambda;
        }

        for (int iteration = 0; ; iteration++) {
            // check max iterations

            if (iteration >= maxiter) {
                m_stopping_reason = 2;
                break;
            }

            options["iteration"] = iteration;

            // compute lhs and rhs

            compute(options);

            // check residual

            m_residual = rhs() - m_target;

            const double rnorm = m_residual.norm();

            std::cout << fmt::format("{:>4} {:}", iteration, rnorm)
                << std::endl;

            // check residual norm

            if (rnorm < rtol) {
                m_stopping_reason = 0;
                break;
            }

            // solve iteration

            m_solver->set_matrix(m_lhs);
            m_solver->solve(m_residual, m_x);

            // update system

            for (int i = 0; i < nb_free_dofs(); i++) {
                m_dofs[i].set_delta(m_dofs[i].delta() - m_x(i));
            }

            // check x norm

            const double xnorm = m_x.norm();

            if (xnorm < xtol) {
                m_stopping_reason = 1;
                break;
            }
        }

        for (int i = 0; i < nb_free_dofs(); i++) {
            m_dofs[i].set_residual(m_residual(i));
        }
    }
};

} // namespace EQlib