/* ----------------------------------------------------------------------------

 * GTSAM Copyright 2010, Georgia Tech Research Corporation,
 * Atlanta, Georgia 30332-0415
 * All Rights Reserved
 * Authors: Frank Dellaert, et al. (see THANKS for the full author list)

 * See LICENSE for the license information

 * -------------------------------------------------------------------------- */

/**
 * @file    ISAM2.h
 * @brief   Incremental update functionality (ISAM2) for BayesTree, with fluid relinearization.
 * @author  Michael Kaess
 */

// \callgraph

#pragma once

#include <map>
#include <list>
#include <vector>
#include <stdexcept>

#include <gtsam/base/types.h>
#include <gtsam/base/FastSet.h>
#include <gtsam/base/FastList.h>
#include <gtsam/inference/FactorGraph.h>
#include <gtsam/nonlinear/NonlinearFactorGraph.h>
#include <gtsam/nonlinear/Ordering.h>
#include <gtsam/inference/BayesNet.h>
#include <gtsam/inference/BayesTree.h>
#include <gtsam/linear/GaussianFactorGraph.h>
#include <gtsam/linear/HessianFactor.h>

namespace gtsam {

/**
 * @defgroup ISAM2
 */

/**
 * @ingroup ISAM2
 * Parameters for the ISAM2 algorithm.  Default parameter values are listed below.
 */
struct ISAM2Params {
  double wildfireThreshold; ///< Continue updating the linear delta only when changes are above this threshold (default: 0.001)
  double relinearizeThreshold; ///< Only relinearize variables whose linear delta magnitude is greater than this threshold (default: 0.1)
  int relinearizeSkip; ///< Only relinearize any variables every relinearizeSkip calls to ISAM2::update (default: 10)
  bool enableRelinearization; ///< Controls whether ISAM2 will ever relinearize any variables (default: true)
  bool evaluateNonlinearError; ///< Whether to evaluate the nonlinear error before and after the update, to return in ISAM2Result from update()

  /** Specify parameters as constructor arguments */
  ISAM2Params(
      double _wildfireThreshold = 0.001, ///< ISAM2Params::wildfireThreshold
      double _relinearizeThreshold = 0.1, ///< ISAM2Params::relinearizeThreshold
      int _relinearizeSkip = 10, ///< ISAM2Params::relinearizeSkip
      bool _enableRelinearization = true, ///< ISAM2Params::enableRelinearization
      bool _evaluateNonlinearError = false ///< ISAM2Params::evaluateNonlinearError
  ) : wildfireThreshold(_wildfireThreshold), relinearizeThreshold(_relinearizeThreshold),
      relinearizeSkip(_relinearizeSkip), enableRelinearization(_enableRelinearization),
      evaluateNonlinearError(_evaluateNonlinearError) {}
};

/**
 * @ingroup ISAM2
 * This struct is returned from ISAM2::update() and contains information about
 * the update that is useful for determining whether the solution is
 * converging, and about how much work was required for the update.  See member
 * variables for details and information about each entry.
 */
struct ISAM2Result {
  /** The nonlinear error of all of the factors, \a including new factors and
   * variables added during the current call to ISAM2::update().  This error is
   * calculated using the following variable values:
   * \li Pre-existing variables will be evaluated by combining their
   * linearization point before this call to update, with their partial linear
   * delta, as computed by ISAM2::calculateEstimate().
   * \li New variables will be evaluated at their initialization points passed
   * into the current call to update.
   * \par Note: This will only be computed if ISAM2Params::evaluateNonlinearError
   * is set to \c true, because there is some cost to this computation.
   */
  boost::optional<double> errorBefore;

  /** The nonlinear error of all of the factors computed after the current
   * update, meaning that variables above the relinearization threshold
   * (ISAM2Params::relinearizeThreshold) have been relinearized and new
   * variables have undergone one linear update.  Variable values are
   * again computed by combining their linearization points with their
   * partial linear deltas, by ISAM2::calculateEstimate().
   * \par Note: This will only be computed if ISAM2Params::evaluateNonlinearError
   * is set to \c true, because there is some cost to this computation.
   */
  boost::optional<double> errorAfter;

  /** The number of variables that were relinearized because their linear
   * deltas exceeded the reslinearization threshold
   * (ISAM2Params::relinearizeThreshold), combined with any additional
   * variables that had to be relinearized because they were involved in
   * the same factor as a variable above the relinearization threshold.
   * On steps where no relinearization is considered
   * (see ISAM2Params::relinearizeSkip), this count will be zero.
   */
  size_t variablesRelinearized;

  /** The number of variables that were reeliminated as parts of the Bayes'
   * Tree were recalculated, due to new factors.  When loop closures occur,
   * this count will be large as the new loop-closing factors will tend to
   * involve variables far away from the root, and everything up to the root
   * will be reeliminated.
   */
  size_t variablesReeliminated;
};

/**
 * @ingroup ISAM2
 * Implementation of the full ISAM2 algorithm for incremental nonlinear optimization.
 *
 * The typical cycle of using this class to create an instance by providing ISAM2Params
 * to the constructor, then add measurements and variables as they arrive using the update()
 * method.  At any time, calculateEstimate() may be called to obtain the current
 * estimate of all variables.
 */
template<class CONDITIONAL, class VALUES>
class ISAM2: public BayesTree<CONDITIONAL> {

protected:

  /** The current linearization point */
  VALUES theta_;

  /** VariableIndex lets us look up factors by involved variable and keeps track of dimensions */
  VariableIndex variableIndex_;

  /** The linear delta from the last linear solution, an update to the estimate in theta */
  VectorValues deltaUnpermuted_;

  /** @brief The permutation through which the deltaUnpermuted_ is
   * referenced.
   *
   * Permuting Vector entries would be slow, so for performance we
   * instead maintain this permutation through which we access the linear delta
   * indirectly
   */
  Permuted<VectorValues> delta_;

  /** All original nonlinear factors are stored here to use during relinearization */
  NonlinearFactorGraph<VALUES> nonlinearFactors_;

  /** @brief The current elimination ordering Symbols to Index (integer) keys.
   *
   * We keep it up to date as we add and reorder variables.
   */
  Ordering ordering_;

  /** The current parameters */
  ISAM2Params params_;

private:
#ifndef NDEBUG
  std::vector<bool> lastRelinVariables_;
#endif

  typedef HessianFactor CacheFactor;

public:

  typedef BayesTree<CONDITIONAL> Base; ///< The BayesTree base class
  typedef ISAM2<CONDITIONAL, VALUES> This; ///< This class

  /** Create an empty ISAM2 instance */
  ISAM2(const ISAM2Params& params);

  /** Create an empty ISAM2 instance using the default set of parameters (see ISAM2Params) */
  ISAM2();

  typedef typename BayesTree<CONDITIONAL>::sharedClique sharedClique; ///< Shared pointer to a clique
  typedef typename BayesTree<CONDITIONAL>::Cliques Cliques; ///< List of Clique typedef from base class

  /**
   * Add new factors, updating the solution and relinearizing as needed.
   *
   * Add new measurements, and optionally new variables, to the current system.
   * This runs a full step of the ISAM2 algorithm, relinearizing and updating
   * the solution as needed, according to the wildfire and relinearize
   * thresholds.
   *
   * @param newFactors The new factors to be added to the system
   * @param newTheta Initialization points for new variables to be added to the system.
   * You must include here all new variables occuring in newFactors (which were not already
   * in the system).  There must not be any variables here that do not occur in newFactors,
   * and additionally, variables that were already in the system must not be included here.
   * @param force_relinearize Relinearize any variables whose delta magnitude is sufficiently
   * large (Params::relinearizeThreshold), regardless of the relinearization interval
   * (Params::relinearizeSkip).
   * @return An ISAM2Result struct containing information about the update
   */
  ISAM2Result update(const NonlinearFactorGraph<VALUES>& newFactors, const VALUES& newTheta,
      bool force_relinearize = false);

  /** Access the current linearization point */
  const VALUES& getLinearizationPoint() const {return theta_;}

  /** Compute an estimate from the incomplete linear delta computed during the last update.
   * This delta is incomplete because it was not updated below wildfire_threshold.  If only
   * a single variable is needed, it is faster to call calculateEstimate(const KEY&).
   */
  VALUES calculateEstimate() const;

  /** Compute an estimate for a single variable using its incomplete linear delta computed
   * during the last update.  This is faster than calling the no-argument version of
   * calculateEstimate, which operates on all variables.
   * @param key
   * @return
   */
  template<class KEY>
  typename KEY::Value calculateEstimate(const KEY& key) const;

  /// @name Public members for non-typical usage
  //@{

  /** Internal implementation functions */
  struct Impl;

  /** Compute an estimate using a complete delta computed by a full back-substitution.
   */
  VALUES calculateBestEstimate() const;

  /** Access the current delta, computed during the last call to update */
  const Permuted<VectorValues>& getDelta() const { return delta_; }

  /** Access the set of nonlinear factors */
  const NonlinearFactorGraph<VALUES>& getFactorsUnsafe() const { return nonlinearFactors_; }

  /** Access the current ordering */
  const Ordering& getOrdering() const { return ordering_; }

  size_t lastAffectedVariableCount;
  size_t lastAffectedFactorCount;
  size_t lastAffectedCliqueCount;
  size_t lastAffectedMarkedCount;
  size_t lastBacksubVariableCount;
  size_t lastNnzTop;

  //@}

private:

  FastList<size_t> getAffectedFactors(const FastList<Index>& keys) const;
  FactorGraph<GaussianFactor>::shared_ptr relinearizeAffectedFactors(const FastList<Index>& affectedKeys) const;
  FactorGraph<CacheFactor> getCachedBoundaryFactors(Cliques& orphans);

  boost::shared_ptr<FastSet<Index> > recalculate(const FastSet<Index>& markedKeys, const FastSet<Index>& structuralKeys,
      const FastVector<Index>& newKeys, const FactorGraph<GaussianFactor>::shared_ptr newFactors, ISAM2Result& result);
  //	void linear_update(const GaussianFactorGraph& newFactors);

}; // ISAM2

} /// namespace gtsam
