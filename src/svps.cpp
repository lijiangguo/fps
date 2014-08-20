//
// svd.cpp
// 
// Created by Vincent Q. Vu on 2014-07-09
// Copyright 2014 Vincent Q. Vu. All rights reserved
// 

#include <RcppArmadillo.h>
#include <cmath>

#include "admm.h"
#include "block/block.h"
#include "graphseq/bigraphseq.h"
#include "lambda.h"
#include "projection.h"
#include "softthreshold.h"
#include "distance.h"

using namespace Rcpp;
using namespace arma;

//' Singular Value Projection and Selection
//'
//' This function computes a solution path of the Singular Value Projection 
//' and Selection (SVPS) estimator.  It takes a data matrix \code{x} as input 
//' and returns an object containing a list of projection matrices 
//' estimated by SVPS over a sequence of regularization parameter values.
//'
//' By default, a sensible minimum value of the regularization parameter 
//' is automatically chosen so that the minimally regularized solution
//' is different from the ordinary SVD.
//'
//' @param x              Input matrix
//' @param ndim           Target subspace dimension (can be fractional)
//' @param nsol           Number of solutions to compute
//' @param maxblocksize   Suggested maximum block size (rows + columns);
//'                       ignored if \code{== 0}
//' @param minblocknum    Suggested minimum number of blocks; ignored 
//'                       if \code{< 2}
//' @param lambdaminratio Minimum value of lambda as a fraction of 
//'                       the automatically determined maximum value of 
//'                       lambda; ignored if \code{< 0}
//' @param lambdamin      Minimum value of lambda; automatically determined 
//'                       if \code{< 0}
//' @param lambda         Vector of regularization parameter values
//' @param maxiter        Maximum number of iterations for each solution
//' @param tolerance      Convergence threshold
//' @param verbose        Level of verbosity; silent if \code{= 0}; 
//'                       otherwise display more messages and progress 
//'                       indicators as \code{verbose} increases
//'
//' @details
//'
//' The default automatic choice of \code{lambdamin} ensures that the least 
//' regularized estimate omits at least one row or column. The solutions 
//' are automatically sorted in decreasing order of \code{lambda}.
//'
//' @return An S3 object of class \code{svps} which is a list with the 
//'         following components:
//'   \item{ndim}{Sum of squares (dimension) of the estimate}
//'   \item{lambda}{Vector containing the regularization parameters of each 
//'                 estimate.}
//'   \item{projection}{List containing the the (bi-)projection matrix 
//'                     estimates}
//'   \item{leverage.row}{Matrix whose columns are the row leverages}
//'   \item{leverage.col}{Matrix whose columns are the column leverages}
//'   \item{L1}{Vector of the sum of absolute values of each estimate}
//'   \item{var.row}{}
//'   \item{var.col}{}
//'   \item{var.total}{}
//'   \item{order.row}{a vector giving a permutation of the row indices of \code{x} so that the projection matrix estimates are in block form}
//'   \item{order.col}{a vector giving a permutation of the column indices of \code{x} so that the projection matrix estimates are in block form}
//'   \item{niter}{a vector containing the number of ADMM iterations for each 
//'                estimate}
//'
//' @export
//'
//' @examples
//' # Apply FPS to the standardized wine data from the UCI ML repository
//' data(wine)
//' out <- svps(scale(wine), ndim = 3)
//' print(out)
//' plot(out)
//'
// [[Rcpp::export]]
List svps(NumericMatrix x, double ndim, unsigned int nsol = 50, 
          unsigned int maxblocksize = 0, unsigned int minblocknum = 2, 
          double lambdaminratio = -1, double lambdamin = -1, 
          NumericVector lambda = NumericVector::create(), 
          int maxiter = 100, double tolerance = 1e-3, int verbose = 0) {

  // Sanity checks
  if (x.ncol() < 2 || x.nrow() < 2) { stop("Expected x to be a matrix"); }
  if (ndim <= 0.0 || ndim >= std::fmin(x.nrow(), x.ncol())) {
    stop("Expected 0 < ndim < min(dim(x))");
  }
  if (nsol < 1) { stop("Expected nsol > 0"); }
  if (maxiter < 1) { stop("Expected maxiter > 0"); }
  if (tolerance <= 0.0) { stop("Expected tolerance > 0"); }

  // Map x to an arma::mat
  const mat _x(x.begin(), x.nrow(), x.ncol(), false);

  vec _lambda;

  if (lambda.size() > 0) {
    _lambda = vec(lambda.begin(), lambda.size());
    std::sort(_lambda.begin(), _lambda.end(), std::greater<double>());
    lambdamin = _lambda[_lambda.n_elem - 1];
    nsol = _lambda.n_elem;
  }

  // There are several ways to indirectly set the minimum value of lambda
  // We define the priority as:
  // lambda > lambdamin > lambdaminratio > (maxblocksize, minblocknum)
  if (lambdaminratio > 0) { maxblocksize = 0; minblocknum = 0; }
  if (lambdamin >= 0) { 
    lambdaminratio = -1; maxblocksize = 0; minblocknum = 0; }

  // Compute the sequence of solution graphs
  BiGraphSeq gs(_x, std::fmax(0.0, lambdamin), 
                maxblocksize > 0 ? (uword) maxblocksize 
                                 : _x.n_rows + _x.n_cols,
                minblocknum);

  // Generate lambda sequence if necessary
  if (lambda.size() == 0) {
    _lambda = compute_lambda(gs, lambdamin, lambdaminratio, nsol);
  }

  // Placeholders for solutions
  List            projection(nsol);
  NumericVector   niter(nsol);
  NumericVector   L1(nsol);
  NumericVector   var_row(nsol);
  NumericVector   var_col(nsol);
  NumericMatrix   leverage_row(_x.n_rows, nsol);
  NumericMatrix   leverage_col(_x.n_cols, nsol);
  mat _leverage_row(leverage_row.begin(), _x.n_rows, nsol, false),
      _leverage_col(leverage_col.begin(), _x.n_cols, nsol, false);

  // Set dimnames attribute of leverages array
  List dimnames(x.attr("dimnames"));
  if (dimnames.size() > 0) {
    leverage_row.attr("dimnames") = List::create(_["variable"] = dimnames[0],
                                                 _["lambda"] = R_NilValue);
    leverage_col.attr("dimnames") = List::create(_["variable"] = dimnames[1],
                                                 _["lambda"] = R_NilValue);
  }

  // ADMM variables
  mat _z(_x.n_rows, _x.n_cols, fill::zeros),
      _u(_x.n_rows, _x.n_cols, fill::zeros);

  // ADMM parameters
  double tolerance_abs = std::sqrt(ndim) * tolerance,
         admm_penalty = gs.size() > 1 ? (++gs.cbegin())->first : 1.0,
         admm_adjust = 2.0;

  // Outer loop to compute solution path
  for (unsigned int i = 0; i < nsol; ++i) {
    if (verbose > 0) { Rcout << "."; }

#ifdef FPS_DONT_USE_GRAPH_OPTIMIZATION
    // ADMM
    niter[i] = admm(SingularValueProjection(ndim), 
                    EntrywiseSoftThreshold(_lambda[i]), 
                    FrobeniusDistance(), 
                    _x, _z, _u, admm_penalty, admm_adjust, maxiter, tolerance_abs);

    // Store solution
    NumericMatrix p(_x.n_rows, _x.n_cols);
    p.attr("dimnames") = x.attr("dimnames");
    mat _p(p.begin(), p.nrow(), p.ncol(), false);
    _p = _z;
    projection(i) = p;

    L1(i) = norm(vectorise(_z), 1);
    var_row(i) = accu(square(_x.t() * _z)); // trace(xx' pp')
    var_col(i) = accu(square(_x * _z.t())); // trace(x'x p'p)
    _leverage_row.col(i) = vectorise(sum(square(_z), 1));
    _leverage_col.col(i) = vectorise(sum(square(_z), 0));
#else
    // Find active vertex partition and construct block matrix
    const BiGraphSeq::partition_t& active = gs.get_active(_lambda[i]);

    if(verbose > 1) { Rcout << active.size() << " blocks"; }

    block::map<BiGraphSeq::partition_t> b_x(_x, active), 
                                        b_z(_z, active), 
                                        b_u(_u, active);

    // ADMM
    niter[i] = admm(SingularValueProjection(ndim), 
                    EntrywiseSoftThreshold(_lambda[i]), 
                    FrobeniusDistance(), 
                    static_cast<block::mat&>(b_x), 
                    static_cast<block::mat&>(b_z), 
                    static_cast<block::mat&>(b_u), 
                    admm_penalty, admm_adjust, maxiter, tolerance_abs);

    // Restore dense matrices
    b_z.copy_to(_z);
    b_u.copy_to(_u);

    // Store solution
    NumericMatrix p(_x.n_rows, _x.n_cols);
    p.attr("dimnames") = x.attr("dimnames");
    mat _p(p.begin(), p.nrow(), p.ncol(), false);
    b_z.copy_to(_p);
    projection(i) = p;

    L1(i) = block::sumabs(b_z);
    var_row(i) = block::dotsquare(b_x, b_z); // trace(xx' pp')
    var_col(i) = block::tdotsquare(b_x, b_z); // trace(x'x p'p)
    _leverage_row.col(i) = vectorise(sum(square(_p), 1));
    _leverage_col.col(i) = vectorise(sum(square(_p), 0));
#endif

    if (verbose > 2) { Rcout << niter[i] << " iterations"; }
  }

  if (verbose > 0) { Rcout << std::endl; }

  // Store ordering of rows/columns
  arma::uvec order_row(_x.n_rows), order_col(_x.n_cols);
  auto i = order_row.begin(), j = order_col.begin();
  for (const auto& b : gs.crbegin()->second) {
    for (auto bi : b.second.first) { *i++ = bi + 1; }
    for (auto bi : b.second.second) { *j++ = bi + 1; }
  }

  // Return
  List out = List::create(
    Named("ndim") = ndim,
    Named("lambda") = wrap(_lambda.memptr(), 
                           _lambda.memptr() + _lambda.n_elem),
    Named("projection") = projection,
    Named("leverage.row") = leverage_row,
    Named("leverage.col") = leverage_col,
    Named("L1") = L1,
    Named("var.row") = var_row,
    Named("var.col") = var_col,
    Named("var.total") = accu(square(_x)),
    Named("order.row") = wrap(order_row.memptr(), 
                              order_row.memptr() + order_row.n_elem),
    Named("order.col") = wrap(order_col.memptr(), 
                              order_col.memptr() + order_col.n_elem),
    Named("niter") = niter
  );
  out.attr("class") = "svps";

  return out;
}
