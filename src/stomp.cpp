#include "stomp.h"
#include "fft.h"
#include "mass.h"
#include "math.h"
#include <numeric>
// [[Rcpp::depends(RcppProgress)]]
#include <progress.hpp>
// [[Rcpp::depends(RcppThread)]]
#include <RcppThread.h>
// [[Rcpp::depends(RcppParallel)]]
#include <RcppParallel.h>
using namespace RcppParallel;

#if RCPP_PARALLEL_USE_TBB
#include "tbb/mutex.h"
#else
#include "tthread/fast_mutex.h"
#endif

// [[Rcpp::export]]
List stomp_rcpp(const NumericVector data_ref, const NumericVector query_ref,
                uint32_t window_size, double ez = 0.5, bool progress = false) {
  bool partial = false;
  double exclusion_zone = round(window_size * ez + DBL_EPSILON);
  uint32_t data_size = data_ref.length();
  uint32_t query_size = query_ref.length();
  uint32_t matrix_profile_size = data_size - window_size + 1;
  uint32_t num_queries = query_size - window_size + 1;

  // check skip position
  LogicalVector skip_location(matrix_profile_size);

  for (uint32_t i = 0; i < matrix_profile_size; i++) {
    NumericVector range = data_ref[Range(i, (i + window_size - 1))];
    if (any(is_na(range) | is_infinite(range))) {
      skip_location[i] = true;
    }
  }

  NumericVector data = data_ref;
  NumericVector query = query_ref;

  data[is_na(data)] = 0;
  data[is_infinite(data)] = 0;
  query[is_na(query)] = 0;
  query[is_infinite(query)] = 0;

  NumericVector matrix_profile(matrix_profile_size, R_PosInf);
  IntegerVector profile_index(matrix_profile_size, R_NegInf);

  uint32_t k = find_best_k_rcpp(data, query, window_size);

  List pre = mass_pre_rcpp(data, query, window_size);
  List rpre = mass_pre_rcpp(query, data, window_size);
  List nn = mass3_rcpp(query[Range(0, window_size - 1)], data, pre["data_size"],
                       rpre["window_size"], pre["data_mean"], pre["data_sd"],
                       as<NumericVector>(pre["query_mean"])[0],
                       as<NumericVector>(pre["query_sd"])[0], k);

  ///// This is needed for JOIN similarity
  List rnn = mass3_rcpp(data[Range(0, window_size - 1)], query, query_size,
                        rpre["window_size"], rpre["data_mean"], rpre["data_sd"],
                        as<NumericVector>(rpre["query_mean"])[0],
                        as<NumericVector>(rpre["query_sd"])[0], k);

  NumericVector first_product = rnn["last_product"];

  ///// This is needed for JOIN similarity
  IntegerVector lp_range = Range(1, (data_size - window_size));
  IntegerVector lp2_range = Range(0, (data_size - window_size - 1));
  IntegerVector dt_range = Range(window_size, data_size - 1);
  NumericVector distance_profile;
  NumericVector last_product;
  double drop_value = query[0];

  IntegerVector order = Range(0, num_queries - 1);

  Progress p(num_queries, progress);

  try {
    for (int32_t i : order) {
      p.increment();

      if (i % 100 == 0) {
        RcppThread::checkUserInterrupt();
      }

      // compute the distance profile
      NumericVector query_window = query[Range(i, (i + window_size - 1))];
      if (i == 0) {
        distance_profile = nn["distance_profile"];
        last_product = nn["last_product"];
      } else {
        last_product[lp_range] = (NumericVector)(
            as<NumericVector>(last_product[lp2_range]) -
            as<NumericVector>(data[lp2_range]) * drop_value +
            as<NumericVector>(data[dt_range]) * query_window[window_size - 1]);
        last_product[0] = first_product[i];
        distance_profile =
            2 *
            (window_size -
             (last_product - window_size * as<NumericVector>(pre["data_mean"]) *
                                 as<NumericVector>(pre["query_mean"])[i]) /
                 (as<NumericVector>(pre["data_sd"]) *
                  as<NumericVector>(pre["query_sd"])[i]));
      }
      distance_profile[distance_profile < 0] = 0;
      // distance_profile = sqrt(distance_profile);
      drop_value = query_window[0];

      // apply exclusion zone
      if (exclusion_zone > 0) {
        uint32_t exc_st = MAX(0, i - exclusion_zone);
        uint32_t exc_ed = MIN(matrix_profile_size - 1, i + exclusion_zone);
        IntegerVector dp_range = Range(exc_st, exc_ed);
        distance_profile[dp_range] = R_PosInf;
      }

      distance_profile[as<NumericVector>(pre["data_sd"]) < DBL_EPSILON] =
          R_PosInf;
      if (skip_location[i] ||
          as<NumericVector>(pre["query_sd"])[i] < DBL_EPSILON) {
        distance_profile.fill(R_PosInf);
      }
      distance_profile[skip_location] = R_PosInf;

      LogicalVector idx = (distance_profile < matrix_profile);
      matrix_profile[idx] = distance_profile[idx];
      profile_index[which(idx)] = i + 1;
    }

    matrix_profile = sqrt(matrix_profile);
  } catch (RcppThread::UserInterruptException &e) {
    partial = true;
    Rcout << "Process terminated by the user successfully, partial results "
             "were returned.";
  } catch (...) {
    ::Rf_error("c++ exception (unknown reason)");
  }

  return (List::create(Rcpp::Named("matrix_profile") = matrix_profile,
                       Rcpp::Named("profile_index") = profile_index,
                       Rcpp::Named("partial") = partial,
                       Rcpp::Named("ez") = ez));
}

template <typename Iterator>
List mass3_cpp(const Iterator query_it, const Iterator data_it,
               const uint64_t data_size, const uint32_t window_size,
               const Iterator data_mean_it, const Iterator data_sd_it,
               const double query_mean, const double query_sd, uint32_t k) {
  // data_it is the long time series query_it is the query k is the size of
  //     pieces,
  //     preferably a power of two data_it is the data,
  //     query_it is the query

  uint32_t w_size = window_size;
  uint64_t d_size = data_size;
  uint64_t p_size = data_size - window_size + 1;

  std::vector<double> dist(p_size);
  std::vector<double>::iterator dist_it = dist.begin();
  std::vector<double> last(p_size);
  std::vector<double>::iterator last_it = last.begin();

  k = set_k_rcpp(k, d_size, w_size);

  // compute query_it stats -- O(d_size)
  double q_mean = query_mean;
  double q_std = query_sd;

  // compute data stats -- O(d_size)
  Iterator d_mean_it = data_mean_it;
  Iterator d_std_it = data_sd_it;

  std::vector<double> rev_query(k);
  std::reverse_copy(query_it, query_it + w_size, rev_query.begin());

  std::vector<std::complex<double>> Y = fft_rcpp(rev_query);

  uint64_t j = 0;
  uint64_t jump = k - w_size + 1;
  uint64_t seq_end = d_size - k;
  std::vector<std::complex<double>> Z(k);
  std::vector<double> z;
  std::vector<double> d(k - w_size + 1);

  try {
    for (j = 0; j <= seq_end; j = j + jump) {
      // The main trick of getting dot products in O(d_size log d_size) time
      uint64_t idx_begin = j;

      std::vector<double> data_chunk(data_it + j, data_it + j + k);
      std::vector<std::complex<double>> X = fft_rcpp(data_chunk);

      std::transform(X.begin(), X.end(), Y.begin(), Z.begin(),
                     std::multiplies<std::complex<double>>());

      z = fft_rcpp_real(Z, true);

      for (uint64_t i = 0; i < (k - w_size + 1); i++) {
        d[i] = 2 * (w_size - (z[w_size - 1 + i] -
                              w_size * *(d_mean_it + idx_begin + i) * q_mean) /
                                 (*(d_std_it + idx_begin + i) * q_std));
      }

      std::copy(d.begin(), d.begin() + k - w_size + 1, dist_it + j);
      std::copy(z.begin() + w_size - 1, z.begin() + k, last_it + j);
    }
  } catch (Rcpp::internal::InterruptedException &ex) {
    std::cout << "Process terminated." << std::endl;
  } catch (...) {
    ::Rf_error("c++ exception (unknown reason)");
  }

  jump = d_size - j;

  try {
    if (jump >= w_size) {
      uint64_t idx_begin = j;

      if ((jump - (w_size - 1) + j) > (uint64_t)p_size) {
        std::cout << "DEBUG: error." << std::endl;
      } else {
        std::vector<double> data_chunk(data_it + j, data_it + d_size);
        std::vector<std::complex<double>> X = fft_rcpp(data_chunk);
        std::vector<double> rev_query_chunk(rev_query.begin(),
                                            rev_query.begin() + jump);
        Y = fft_rcpp(rev_query_chunk);

        Z = std::vector<std::complex<double>>(Y.size());

        std::transform(X.begin(), X.end(), Y.begin(), Z.begin(),
                       std::multiplies<std::complex<double>>());

        z = fft_rcpp_real(Z, true);

        for (uint64_t i = 0; i < (jump - w_size + 1); i++) {
          d[i] =
              2 * (w_size - (z[i + w_size - 1] -
                             w_size * *(d_mean_it + idx_begin + i) * q_mean) /
                                (*(d_std_it + idx_begin + i) * q_std));
        }

        std::copy(d.begin(), d.begin() + (jump - w_size + 1), dist_it + j);
        std::copy(z.begin() + w_size - 1, z.begin() + jump, last_it + j);
      }
    }
  } catch (Rcpp::internal::InterruptedException &ex) {
    std::cout << "Process terminated." << std::endl;
  } catch (...) {
    ::Rf_error("c++ exception (unknown reason)");
  }

  return (List::create(Rcpp::Named("distance_profile") = dist,
                       Rcpp::Named("last_product") = last));
}

struct StompWorker : public Worker {
  // input
  const RVector<double> data_ref;
  const RVector<double> window_ref;
  const uint64_t w_size;
  const uint64_t d_size;
  const RVector<double> d_mean;
  const RVector<double> d_std;
  const RVector<double> q_mean;
  const RVector<double> q_std;
  const RVector<int> skip_location;
  const RVector<double> first_product;
  const uint64_t ez;

  Progress *p;

  RVector<double> mp;
  RVector<int> pi;

  tbb::mutex m;

  // initialize from Rcpp input and output matrixes (the RMatrix class
  // can be automatically converted to from the Rcpp matrix type)
  StompWorker(const NumericVector data_ref, const NumericVector window_ref,
              const uint64_t w_size, const uint64_t d_size,
              const NumericVector d_mean, const NumericVector d_std,
              const NumericVector q_mean, const NumericVector q_std,
              const IntegerVector skip_location,
              const NumericVector first_product, const uint64_t ez, Progress *p,
              NumericVector mp, IntegerVector pi)
      : data_ref(data_ref), window_ref(window_ref), w_size(w_size),
        d_size(d_size), d_mean(d_mean), d_std(d_std), q_mean(q_mean),
        q_std(q_std), skip_location(skip_location),
        first_product(first_product), ez(ez), p(p), mp(mp), pi(pi) {}

  ~StompWorker() {}

  // function call operator that work for the specified range (begin/end)
  void operator()(std::size_t begin, std::size_t end) {
    // begin and end are the query window

    // index of sliding window
    try {
      RcppThread::checkUserInterrupt();

      uint64_t chunk = (end - begin);

      if (chunk <= w_size) {
        std::cout << "Chunk size is too small." << std::endl;
        return;
      }

      uint64_t k = set_k_rcpp(w_size * 2, chunk, w_size);

      m.lock();
      List nn = mass3_cpp(window_ref.begin() + begin, data_ref.begin(), d_size,
                          w_size, d_mean.begin(), d_std.begin(), q_mean[begin],
                          q_std[begin], k);
      m.unlock();

      std::vector<double> distance_profile(d_mean.size());
      std::vector<double> matrix_profile(d_mean.size(), R_PosInf);
      std::vector<int> profile_index(d_mean.size(), -1);
      std::vector<double> last_product(d_mean.size());
      double drop_value = 0;
      uint32_t exc_st = 0;
      uint32_t exc_ed = 0;

      for (uint64_t i = begin; i < end; i++) {

        if (i % 100 == 0) {
          RcppThread::checkUserInterrupt();
          m.lock();
          p->increment();
          m.unlock();
        }

        // compute the distance profile
        if (i == begin) {
          std::copy(as<NumericVector>(nn["distance_profile"]).begin(),
                    as<NumericVector>(nn["distance_profile"]).end(),
                    distance_profile.begin());
          std::copy(as<NumericVector>(nn["last_product"]).begin(),
                    as<NumericVector>(nn["last_product"]).end(),
                    last_product.begin());
        } else {

          for (uint64_t j = d_mean.size() - 1; j > 0; j--) {
            last_product[j] =
                last_product[j - 1] - data_ref[j - 1] * drop_value +
                data_ref[w_size + j - 1] * window_ref[i + w_size - 1];
          }
          last_product[0] = first_product[i];

          if (ez > 0) {
            exc_st = MAX(0, i > ez ? (i - ez) : 0);
            exc_ed = MIN(d_std.size() - 1, i + ez);
          }

          for (uint64_t j = 0; j < d_mean.size(); j++) {
            double dp = R_PosInf;

            if (skip_location[j] == 0) {
              if (ez == 0 || j < exc_st || j > exc_ed) {

                dp = 2 * (w_size -
                          (last_product[j] - w_size * d_mean[j] * q_mean[i]) /
                              (d_std[j] * q_std[i]));
              }
            }

            distance_profile[j] = (dp > 0) ? dp : 0;
          }
        }

        drop_value = window_ref[i];

        for (uint64_t j = 0; j < d_mean.size(); j++) {
          if (distance_profile[j] < matrix_profile[j]) {
            matrix_profile[j] = distance_profile[j];
            profile_index[j] = i + 1;
          }
        }
      }

      m.lock();
      for (uint64_t j = 0; j < d_mean.size(); j++) {
        if (matrix_profile[j] < mp[j]) {
          mp[j] = matrix_profile[j];
          pi[j] = profile_index[j];
        }
      }
      m.unlock();

    } catch (RcppThread::UserInterruptException &e) {
      Rcout << "Computation interrupted by the user." << std::endl;
      Rcout << "Please wait for other threads to stop." << std::endl;
      throw;
    }
  }
};

// [[Rcpp::export]]
List stomp_rcpp_parallel(const NumericVector data_ref,
                         const NumericVector query_ref, uint32_t window_size,
                         double ez = 0.5, bool progress = false) {
  uint64_t exclusion_zone = round(window_size * ez + DBL_EPSILON);
  uint64_t data_size = data_ref.length();
  uint64_t query_size = query_ref.length();
  uint64_t matrix_profile_size = data_size - window_size + 1;
  uint64_t num_queries = query_size - window_size + 1;
  bool partial = false;

  // TODO: check skip position
  IntegerVector skip_location(matrix_profile_size, 0);

  for (uint64_t i = 0; i < matrix_profile_size; i++) {
    NumericVector range = data_ref[Range(i, (i + window_size - 1))];
    if (any(is_na(range) | is_infinite(range))) {
      skip_location[i] = 1;
    }
  }

  NumericVector data = data_ref;
  NumericVector query = query_ref;

  data[is_na(data)] = 0;
  data[is_infinite(data)] = 0;
  query[is_na(query)] = 0;
  query[is_infinite(query)] = 0;

  NumericVector matrix_profile(matrix_profile_size, R_PosInf);
  IntegerVector profile_index(matrix_profile_size, -1);

  uint64_t k = set_k_rcpp(256, data_size, window_size);

  ///// This is needed for JOIN similarity
  List rpre = mass_pre_rcpp(query, data, window_size);
  List rnn = mass3_rcpp(data[Range(0, window_size - 1)], query, query_size,
                        rpre["window_size"], rpre["data_mean"], rpre["data_sd"],
                        as<NumericVector>(rpre["query_mean"])[0],
                        as<NumericVector>(rpre["query_sd"])[0], k);

  NumericVector first_product = rnn["last_product"];

  List pre = mass_pre_rcpp(data, query, window_size);

  Progress p(num_queries / 100, progress);

  StompWorker stomp_worker(data, query, pre["window_size"], data_size,
                           pre["data_mean"], pre["data_sd"], pre["query_mean"],
                           pre["query_sd"], skip_location, first_product,
                           exclusion_zone, &p, matrix_profile, profile_index);

  k = set_k_rcpp(1024, num_queries, window_size);

  // call parallelFor to do the work
  try {
    RcppParallel::parallelFor(0, num_queries, stomp_worker, 2 * k);
  } catch (RcppThread::UserInterruptException &e) {
    partial = true;
    Rcout << "Process terminated by the user successfully, partial results "
             "were returned.";
  } catch (...) {
    ::Rf_error("c++ exception (unknown reason)");
  }

  return (List::create(Rcpp::Named("matrix_profile") = sqrt(matrix_profile),
                       Rcpp::Named("profile_index") = profile_index,
                       Rcpp::Named("partial") = partial,
                       Rcpp::Named("ez") = ez));
}
