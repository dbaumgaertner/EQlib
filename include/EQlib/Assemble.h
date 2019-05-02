#pragma once

#include "Define.h"
#include "Timer.h"

#include <tbb/blocked_range.h>
#include <tbb/enumerable_thread_specific.h>
#include <tbb/iterators.h>
#include <tbb/task_scheduler_init.h>
#include <tbb/parallel_reduce.h>

namespace EQlib {

struct Assemble
{
    static inline tbb::enumerable_thread_specific<Vector> m_lhs_values;
    static inline tbb::enumerable_thread_specific<Vector> m_rhs_values;

    Map<Sparse> m_lhs;
    Map<Vector> m_rhs;

    static inline Map<Sparse> create_lhs(Assemble& s) {
        auto& lhs_values = m_lhs_values.local();

        lhs_values.resize(s.m_lhs.nonZeros());
        lhs_values.setZero();

        return Map<Sparse>(s.m_lhs.rows(), s.m_lhs.cols(), s.m_lhs.nonZeros(),
            s.m_lhs.outerIndexPtr(), s.m_lhs.innerIndexPtr(),
            lhs_values.data());
    }

    static inline Map<Vector> create_rhs(Assemble& s) {
        auto& rhs_values = m_rhs_values.local();

        rhs_values.resize(s.m_rhs.size());
        rhs_values.setZero();

        return Map<Vector>(rhs_values.data(), s.m_rhs.size());
    }

    Assemble(Sparse& lhs, Vector& rhs)
    : m_lhs(lhs.rows(), lhs.cols(), lhs.nonZeros(), lhs.outerIndexPtr(),
        lhs.innerIndexPtr(), lhs.valuePtr())
    , m_rhs(rhs.data(), rhs.size())
    {
        // set lhs and rhs to zero
        Map<Vector>(m_lhs.valuePtr(), m_lhs.nonZeros()).setZero();
        m_rhs.setZero();
    }

    Assemble(Assemble& s, tbb::split)
    : m_lhs(create_lhs(s))
    , m_rhs(create_rhs(s))
    { }

    template <typename TRange>
    void operator()(const TRange& range)
    {
        // compute and add local lhs and rhs

        for (auto it = range.begin(); it != range.end(); ++it) {
            const auto& [element, dof_indices] = *it;

            const auto [local_lhs, local_rhs] = element->compute();

            const size_t nb_dofs = dof_indices.size();

            for (size_t row = 0; row < nb_dofs; row++) {
                const auto row_index = dof_indices[row];

                if (row_index.global >= m_rhs.size()) {
                    continue;
                }

                m_rhs(row_index.global) += local_rhs(row_index.local);

                for (size_t col = row; col < nb_dofs; col++) {
                    const auto col_index = dof_indices[col];

                    if (col_index.global >= m_rhs.size()) {
                        continue;
                    }

                    m_lhs.coeffRef(row_index.global, col_index.global) +=
                        local_lhs(row_index.local, col_index.local);
                }
            }
        }
    }

    void join(Assemble& rhs)
    {
        Map<Vector>(m_lhs.valuePtr(), m_lhs.nonZeros()) +=
            Map<Vector>(rhs.m_lhs.valuePtr(), rhs.m_lhs.nonZeros());
        Map<Vector>(m_rhs.data(), m_rhs.size()) += 
            Map<Vector>(rhs.m_rhs.data(), rhs.m_rhs.size());
    }

    template <typename TElements, typename TIndices>
    static void parallel(
        int nb_threads,
        TElements& elements,
        TIndices& indices,
        Sparse& lhs,
        Vector& rhs)
    {
        py::gil_scoped_release release;

        tbb::task_scheduler_init init(nb_threads > 0 ? nb_threads : tbb::task_scheduler_init::automatic);

        using Iterator = tbb::zip_iterator<TElements::iterator,
            TIndices::iterator>;

        auto begin = tbb::make_zip_iterator(elements.begin(), indices.begin());
        auto end = tbb::make_zip_iterator(elements.end(), indices.end());

        Assemble result(lhs, rhs);

        tbb::parallel_reduce(tbb::blocked_range<Iterator>(begin, end), result);
    }
};

} // namespace EQlib