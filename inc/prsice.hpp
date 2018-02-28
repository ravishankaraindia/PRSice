// This file is part of PRSice2.0, copyright (C) 2016-2017
// Shing Wan Choi, Jack Euesden, Cathryn M. Lewis, Paul F. O’Reilly
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#ifndef PRSICE_H
#define PRSICE_H

#include "commander.hpp"
#include "genotype.hpp"
#include "misc.hpp"
#include "plink_common.hpp"
#include "region.hpp"
#include "regression.hpp"
#include "reporter.hpp"
#include "snp.hpp"
#include "storage.hpp"
#include <Eigen/Dense>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <map>
#include <math.h>
#include <mutex>
#include <random>
#include <stdexcept>
#include <stdio.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#define pthread_t HANDLE
#define THREAD_RET_TYPE unsigned __stdcall
#define THREAD_RETURN return 0
#define EOLN_STR "\r\n"
#else
#include <pthread.h>
#define THREAD_RET_TYPE void*
#define THREAD_RETURN return nullptr
#endif

// This should be the class to handle all the procedures
class PRSice
{
public:
    PRSice(const std::string& base_name, const Commander& commander,
           const bool prset, const size_t sample_ct, Reporter& reporter,
           const size_t num_threshold)
        : m_ignore_fid(commander.ignore_fid())
        , m_prset(prset)
        , m_num_perm(commander.permutation())
        , m_score(commander.get_score())
        , m_missing_score(commander.get_missing_score())
        , m_base_name(base_name)
        , m_target(commander.target_name())
        , m_out(commander.out())
        , m_target_binary(commander.is_binary())
    {
        // might want to use some sort of memory management here
        // If we can store score for more than one p-value threshold,
        // then essentially we reduce the number of time we need to
        // jump around in the binary file / reduce I/O
        int mem_per_threshold = sample_ct * 8; // in byte
        // only use third of the memory available (or the minimum amount
        // for one threshold
        gen_prs_memory(mem_per_threshold, reporter, num_threshold);
        g_logit_perm = commander.logit_perm();
        // we calculate the number of permutation we can run at one time
        bool perm = (commander.permutation() > 0);
        m_seed = commander.seed();
        // check if there's any binary phenotype
        bool has_binary = false;
        for (auto&& b : m_target_binary) {
            if (b) {
                has_binary = true;
                break;
            }
        }
        if (perm) {
            // first check for ridiculously large sample size
            // allow 1 GB here
            if (CHAR_BIT * sample_ct > 100000000) {
                m_perm_per_slice = 1;
            }
            else
            {
                // in theory, most of the time, perm_per_slice should be
                // equal to c_commander.num_permutation();

                int sample_memory = CHAR_BIT * sample_ct;
                m_perm_per_slice = 100000000 / sample_memory;
                m_perm_per_slice = (m_perm_per_slice > m_num_perm)
                                       ? m_num_perm
                                       : m_perm_per_slice;
                // Additional slice to keep
                m_remain_slice = m_num_perm % m_perm_per_slice;
            }
            if (has_binary) {
                if (!g_logit_perm) {
                    std::string message =
                        "Warning: To speed up the permutation, "
                        "we perform  linear regression instead of logistic "
                        "regression within the permutation and uses the "
                        "p-value to rank the thresholds. Our assumptions "
                        "are as follow:\n";
                    message.append("         1. Linear Regression & Logistic "
                                   "Regression produce similar p-values\n");
                    message.append(
                        "         2. P-value is correlated with R2\n\n");
                    message.append("         If you must, you can run logistic "
                                   "regression instead by setting the "
                                   "--logit-perm flag\n\n");
                    reporter.report(message);
                }
                else
                {
                    std::string message = "Warning: Using --logit-perm can be "
                                          "ridiculously slow\n";
                    reporter.report(message);
                }
            }
        }
    };

    virtual ~PRSice();
    void pheno_check(const Commander& c_commander, Reporter& reporter);
    // init_matrix whenever phenotype changes
    void init_matrix(const Commander& c_commander, const size_t pheno_index,
                     Genotype& target, Reporter& reporter,
                     const bool prslice = false);
    size_t num_phenotype() const
    {
        return (pheno_info.use_pheno) ? pheno_info.name.size() : 1;
    };
    void run_prsice(const Commander& c_commander,
                    const std::string& region_name, const size_t pheno_index,
                    const size_t region_index, Genotype& target);
    void regress_score(Genotype& target, const double threshold, size_t thread,
                       const size_t pheno_index, const size_t iter_threshold);

    void prsice(const Commander& c_commander, const Region& c_region,
                const size_t c_pheno_index, bool prslice = false);
    void output(const Commander& c_commander, const Region& region,
                const size_t pheno_index, const size_t region_index,
                Genotype& target);
    void transpose_all(const Commander& c_commander, const Region& c_region,
                       size_t pheno_index) const;
    void summarize(const Commander& c_commander, Reporter& reporter);

protected:
private:
    struct prsice_result
    {
        double threshold;
        double r2;
        double r2_adj;
        double coefficient;
        double p;
        double emp_p;
        double se;
        int num_snp;
    };

    struct prsice_summary
    {
        prsice_result result;
        std::string pheno;
        std::string set;
        double r2_null;
        double top;
        double bottom;
        double prevalence;
    };

    struct Pheno_Info
    {
        std::vector<int> col;
        std::vector<std::string> name;
        std::vector<int> order;
        std::vector<bool> binary;
        bool use_pheno;
    } pheno_info;


    bool m_ignore_fid = false;
    bool m_prset = false;
    static bool g_logit_perm;
    Eigen::VectorXd m_phenotype;
    double m_null_r2 = 0.0;
    double m_null_p = 1.0;
    double m_null_se = 0.0;
    double m_null_coeff = 0.0;
    int m_best_index = -1;
    int m_perm_per_slice = 0;
    int m_remain_slice = 0;
    unsigned int m_seed = 0;
    size_t m_num_perm = 0;
    size_t m_num_snp_included = 0;
    size_t m_region_index = 0;
    size_t m_all_thresholds = 0;
    size_t m_max_fid_length = 3;
    size_t m_max_iid_length = 3;
    SCORING m_score = SCORING::AVERAGE;
    MISSING_SCORE m_missing_score = MISSING_SCORE::MEAN_IMPUTE;
    std::string m_log_file;
    std::string m_base_name;
    std::string m_target;
    std::string m_out;
    std::vector<bool> m_target_binary;
    std::vector<prsice_result> m_prs_results;
    std::vector<prsice_summary> m_prs_summary; // for multiple traits
    std::vector<double> m_best_sample_score;
    std::vector<size_t> m_sample_index;
    std::vector<size_t> m_significant_store{0, 0, 0}; // store the number of
                                                      // non-sig, margin sig,
                                                      // and sig pathway &
                                                      // phenotype

    std::unordered_map<std::string, size_t> m_sample_with_phenotypes;

    // pthread mutli_thread require stuff?
    struct perm_info
    {
        size_t start;
        size_t end;
        size_t processed;
        size_t rank;
    };

    // Global Stuff (Either for threading or for memory control)
    static Eigen::MatrixXd g_independent_variables;
    static std::vector<double> g_perm_result;
    static std::unordered_map<uintptr_t, perm_info> g_perm_range;
    static Eigen::ColPivHouseholderQR<Eigen::MatrixXd> g_perm_pre_decomposed;
    static std::vector<Eigen::MatrixXd> g_permuted_pheno;
    static Eigen::VectorXd g_pre_se_calulated;
    static unsigned char* g_prs_storage_memory;
    static int g_max_threshold_store;
    // Functions
    void thread_score(size_t region_start, size_t region_end, double threshold,
                      size_t thread, const size_t c_pheno_index,
                      const size_t iter_threshold);
    static THREAD_RET_TYPE thread_perm(void* id);

    void permutation(Genotype& target, const size_t n_thread, bool logit_perm);
    void update_sample_included(Genotype& target);
    void gen_pheno_vec(Genotype& target, const std::string& pheno_file_name,
                       const int pheno_index, bool regress, Reporter& reporter);
    std::vector<size_t> get_cov_index(const std::string& c_cov_file,
                                      std::vector<std::string>& cov_header,
                                      Reporter& reporter);
    void gen_cov_matrix(const std::string& c_cov_file,
                        std::vector<std::string>& cov_header,
                        Reporter& reporter);
    void check_factor_cov(
        const std::string& c_cov_file,
        const std::vector<std::string>& c_cov_header,
        const std::vector<size_t>& cov_index,
        std::vector<std::unordered_map<std::string, int>>& factor_levels);
    // This should help us to update the m_prs_results
    void process_permutations();
    void summary();

    void gen_prs_memory(int min_memory_byte, Reporter& reporter, size_t const max_threshold)
    {
    		intptr_t max_req_memory = min_memory_byte*max_threshold;
#ifdef __APPLE__
        int32_t mib[2];
        size_t sztmp;
#endif
        unsigned char* bigstack_ua = nullptr; // ua = unaligned
        int64_t llxx;
        intptr_t default_alloc_mb;
        intptr_t malloc_size_mb = 0;
#ifdef __APPLE__
        mib[0] = CTL_HW;
        mib[1] = HW_MEMSIZE;
        llxx = 0;

        sztmp = sizeof(int64_t);
        sysctl(mib, 2, &llxx, &sztmp, nullptr, 0);
        llxx /= 1048576;
#else
#ifdef _WIN32
        MEMORYSTATUSEX memstatus;
        memstatus.dwLength = sizeof(memstatus);
        GlobalMemoryStatusEx(&memstatus);
        llxx = memstatus.ullTotalPhys / 1048576;
#else
        llxx = ((uint64_t) sysconf(_SC_PHYS_PAGES))
               * ((size_t) sysconf(_SC_PAGESIZE)) / 1048576;
#endif
#endif
        if (!llxx) {
            default_alloc_mb = BIGSTACK_DEFAULT_MB;
        }
        else if (llxx < (BIGSTACK_MIN_MB * 2))
        {
            default_alloc_mb = BIGSTACK_MIN_MB;
        }
        else
        {
            default_alloc_mb = llxx / 2;
        }
        if (!malloc_size_mb) {
            malloc_size_mb = default_alloc_mb;
        }
        else if (malloc_size_mb < BIGSTACK_MIN_MB)
        {
            malloc_size_mb = BIGSTACK_MIN_MB;
        }
        std::string message = "";
#ifndef __LP64__
        if (malloc_size_mb > 2047) {
            malloc_size_mb = 2047;
        }
#endif
        if (llxx) {
            message = std::to_string(llxx) + " MB RAM detected; reserving "
                      + std::to_string(malloc_size_mb) + " MB for PRS\n";
        }
        else
        {
            message = "Failed to calculate system memory. Attemping to reserve"
                      + std::to_string(malloc_size_mb) + " MB for PRS storage\n";
        }
        bigstack_ua =
            (unsigned char*) malloc(malloc_size_mb * 1048576 * sizeof(char));
        // if fail, return nullptr which will then get into the while loop
        while (!bigstack_ua) {
            malloc_size_mb = (malloc_size_mb * 3) / 4;
            if (malloc_size_mb < BIGSTACK_MIN_MB) {
                malloc_size_mb = BIGSTACK_MIN_MB;
            }
            bigstack_ua = (unsigned char*) malloc(malloc_size_mb * 1048576
                                                  * sizeof(char));
            if (bigstack_ua) {
                message.append(
                    "Allocated " + std::to_string(malloc_size_mb)
                    + " MB successfully, after larger attempt(s) failed\n");
            }
            else if (malloc_size_mb == BIGSTACK_MIN_MB)
            {
                throw std::runtime_error("Failed to allocate required memory");
            }
        }
        if (malloc_size_mb * 1048576 < min_memory_byte) {
            throw std::runtime_error(
                "Failed to allocate required memory for PRS storage");
        }
        delete[] bigstack_ua;
        bigstack_ua = nullptr;
        std::cerr << message << std::endl;
        intptr_t final_mb = malloc_size_mb / 3;
        if (final_mb * 1048576 < min_memory_byte) {
            final_mb = min_memory_byte;
            g_max_threshold_store = 1;
        }
        else if(final_mb * 1048576 > max_req_memory){
        		final_mb = max_req_memory;
        		g_max_threshold_store = max_threshold;
        }
        else
        {
            // because of C++'s int operation, this will round down, therefore
            // reduce the memory require by tiny bit
            g_max_threshold_store = final_mb * 1048576 / min_memory_byte;
            final_mb = g_max_threshold_store * min_memory_byte;
        }
        g_prs_storage_memory = (unsigned char*) malloc(final_mb * sizeof(char));
        if (!g_prs_storage_memory) {
            throw std::runtime_error(
                "Failed to allocate required memory for PRS storage");
        }
        message = "Reserving " + std::to_string(final_mb / 1048576)
                              + " MB for PRS storage.";
        reporter.report(message);
        /**
         * In fact, the biggest difficulty of using memory pool in the PRSice class is down
         * to a number of problem:
         * 1. Regression might require memory for temporary objects (looking at GLM)
         * 2. Permutation will also need some memory for some objects (e.g. pre-decomposed matrix)
         * 3. We still need memory for all the global variables and the matrix
         * 4. We need two matrix for PRS temp store (Num SNP and The score)
         * And I am not certain I can handle these memory nicely as I am not familiar with
         * memory pool
         *
         * Thing is, we can still experience memory error if user provide too many samples /
         * too many covariates. In that case, we might still crash without any proper warnings
         * Also, while we can test the amount of memory available like PLINK, unless we use the
         * memory pool method like PLINK, we can't be certain that the memory is actually available
         * when we finally try to use them
         */
    }
};

#endif // PRSICE_H
