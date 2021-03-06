/*
 * Copyright Elasticsearch B.V. and/or licensed to Elasticsearch B.V. under one
 * or more contributor license agreements. Licensed under the Elastic License;
 * you may not use this file except in compliance with the Elastic License.
 */

#include <maths/CBoostedTreeImpl.h>

#include <core/CContainerPrinter.h>
#include <core/CImmutableRadixSet.h>
#include <core/CLogger.h>
#include <core/CLoopProgress.h>
#include <core/CPersistUtils.h>
#include <core/CProgramCounters.h>
#include <core/CStopWatch.h>

#include <maths/CBasicStatisticsPersist.h>
#include <maths/CBayesianOptimisation.h>
#include <maths/CBoostedTree.h>
#include <maths/CBoostedTreeFactory.h>
#include <maths/CBoostedTreeLeafNodeStatistics.h>
#include <maths/CBoostedTreeLoss.h>
#include <maths/CBoostedTreeUtils.h>
#include <maths/CDataFrameAnalysisInstrumentationInterface.h>
#include <maths/CDataFrameCategoryEncoder.h>
#include <maths/CQuantileSketch.h>
#include <maths/CSampling.h>
#include <maths/CSetTools.h>
#include <maths/CTreeShapFeatureImportance.h>

#include <boost/circular_buffer.hpp>

#include <algorithm>
#include <memory>

namespace ml {
namespace maths {
using namespace boosted_tree;
using namespace boosted_tree_detail;
using TStrVec = CBoostedTreeImpl::TStrVec;
using TRowItr = core::CDataFrame::TRowItr;
using TMeanVarAccumulator = CBoostedTreeImpl::TMeanVarAccumulator;
using TMemoryUsageCallback = CDataFrameAnalysisInstrumentationInterface::TMemoryUsageCallback;

namespace {
// It isn't critical to recompute splits every tree we add because random
// downsampling means they're only approximate estimates of the full data
// quantiles anyway. So we amortise their compute cost w.r.t. training trees
// by only refreshing once every MINIMUM_SPLIT_REFRESH_INTERVAL trees we add.
const double MINIMUM_SPLIT_REFRESH_INTERVAL{3.0};
const std::string HYPERPARAMETER_OPTIMIZATION_ROUND{"hyperparameter_optimization_round_"};
const std::string TRAIN_FINAL_FOREST{"train_final_forest"};

//! \brief Record the memory used by a supplied object using the RAII idiom.
class CScopeRecordMemoryUsage {
public:
    template<typename T>
    CScopeRecordMemoryUsage(const T& object, TMemoryUsageCallback&& recordMemoryUsage)
        : m_RecordMemoryUsage{std::move(recordMemoryUsage)},
          m_MemoryUsage(core::CMemory::dynamicSize(object)) {
        m_RecordMemoryUsage(m_MemoryUsage);
    }

    ~CScopeRecordMemoryUsage() { m_RecordMemoryUsage(-m_MemoryUsage); }

    CScopeRecordMemoryUsage(const CScopeRecordMemoryUsage&) = delete;
    CScopeRecordMemoryUsage& operator=(const CScopeRecordMemoryUsage&) = delete;

    template<typename T>
    void add(const T& object) {
        std::int64_t memoryUsage(core::CMemory::dynamicSize(object));
        m_MemoryUsage += memoryUsage;
        m_RecordMemoryUsage(memoryUsage);
    }

    template<typename T>
    void remove(const T& object) {
        std::int64_t memoryUsage(core::CMemory::dynamicSize(object));
        m_MemoryUsage -= memoryUsage;
        m_RecordMemoryUsage(-memoryUsage);
    }

private:
    TMemoryUsageCallback m_RecordMemoryUsage;
    std::int64_t m_MemoryUsage;
};

//! \brief Manages exiting from the loop adding trees to the forest.
//!
//! DESCRIPTION:\n
//! Typically, the test error will decrease exponentially to some minimum then
//! slightly increase thereafter as more trees are added. The logic for exiting
//! training a forest is simple: continue to add trees for some fraction of the
//! maximum forest size after we see the smallest test loss. This amounts to a
//! fixed relative runtime penalty for ensuring we don't stop too early since
//! we record the forest size corresponding to the minimum test loss and simply
//! discard the extra trees at the end of training.
class CTrainForestStoppingCondition {
public:
    CTrainForestStoppingCondition(std::size_t maximumNumberTrees)
        : m_MaximumNumberTrees{maximumNumberTrees},
          m_MaximumNumberTreesWithoutImprovement{std::max(
              static_cast<std::size_t>(0.075 * static_cast<double>(maximumNumberTrees) + 0.5),
              std::size_t{1})} {}

    std::size_t bestSize() const { return m_BestTestLoss[0].second; }

    double bestLoss() const { return m_BestTestLoss[0].first; }

    template<typename FUNC>
    bool shouldStop(std::size_t numberTrees, FUNC computeLoss) {
        double loss{computeLoss()};
        m_BestTestLoss.add({loss, numberTrees});
        LOG_TRACE(<< "test loss = " << loss);
        if (numberTrees - m_BestTestLoss[0].second > m_MaximumNumberTreesWithoutImprovement) {
            return true;
        }
        return numberTrees > m_MaximumNumberTrees;
    }

private:
    using TDoubleSizePrMinAccumulator =
        CBasicStatistics::SMin<std::pair<double, std::size_t>>::TAccumulator;

private:
    std::size_t m_MaximumNumberTrees;
    std::size_t m_MaximumNumberTreesWithoutImprovement;
    TDoubleSizePrMinAccumulator m_BestTestLoss;
};

double lossAtNSigma(double n, const TMeanVarAccumulator& lossMoments) {
    return CBasicStatistics::mean(lossMoments) +
           n * std::sqrt(CBasicStatistics::variance(lossMoments));
}

double trace(std::size_t columns, const TMemoryMappedFloatVector& upperTriangle) {
    // This assumes the upper triangle of the matrix is stored row major.
    double result{0.0};
    for (int i = 0, j = static_cast<int>(columns);
         i < upperTriangle.size() && j > 0; i += j, --j) {
        result += upperTriangle(i);
    }
    return result;
}

CDataFrameTrainBoostedTreeInstrumentationStub INSTRUMENTATION_STUB;
}

CBoostedTreeImpl::CBoostedTreeImpl(std::size_t numberThreads,
                                   CBoostedTree::TLossFunctionUPtr loss,
                                   TAnalysisInstrumentationPtr instrumentation)
    : m_NumberThreads{numberThreads}, m_Loss{std::move(loss)},
      m_BestHyperparameters{
          m_Regularization,       m_DownsampleFactor,   m_Eta,
          m_EtaGrowthRatePerTree, m_MaximumNumberTrees, m_FeatureBagFraction},
      m_Instrumentation{instrumentation != nullptr ? instrumentation : &INSTRUMENTATION_STUB} {
}

CBoostedTreeImpl::CBoostedTreeImpl() = default;

CBoostedTreeImpl::~CBoostedTreeImpl() = default;

CBoostedTreeImpl& CBoostedTreeImpl::operator=(CBoostedTreeImpl&&) = default;

void CBoostedTreeImpl::train(core::CDataFrame& frame,
                             const TTrainingStateCallback& recordTrainStateCallback) {

    if (m_DependentVariable >= frame.numberColumns()) {
        HANDLE_FATAL(<< "Internal error: dependent variable '" << m_DependentVariable
                     << "' was incorrectly initialized. Please report this problem.")
        return;
    }
    if (m_Loss == nullptr) {
        HANDLE_FATAL(<< "Internal error: must supply a loss function for training. "
                     << "Please report this problem.")
    }

    if (m_Loss->isRegression()) {
        m_Instrumentation->type(CDataFrameTrainBoostedTreeInstrumentationInterface::E_Regression);
    } else {
        m_Instrumentation->type(CDataFrameTrainBoostedTreeInstrumentationInterface::E_Classification);
    }

    LOG_TRACE(<< "Main training loop...");

    m_TrainingProgress.progressCallback(m_Instrumentation->progressCallback());

    std::int64_t lastMemoryUsage(this->memoryUsage());

    core::CPackedBitVector allTrainingRowsMask{this->allTrainingRowsMask()};
    core::CPackedBitVector noRowsMask{allTrainingRowsMask.size(), false};

    this->startProgressMonitoringFineTuneHyperparameters();

    if (this->canTrain() == false) {

        // Fallback to using the constant predictor which minimises the loss.

        this->startProgressMonitoringFinalTrain();
        m_BestForest.assign(1, this->initializePredictionsAndLossDerivatives(
                                   frame, allTrainingRowsMask, noRowsMask));
        m_BestForestTestLoss = this->meanLoss(frame, allTrainingRowsMask);
        LOG_TRACE(<< "Test loss = " << m_BestForestTestLoss);

    } else if (m_CurrentRound < m_NumberRounds || m_BestForest.empty()) {
        TMeanVarAccumulator timeAccumulator;
        core::CStopWatch stopWatch;
        stopWatch.start();
        std::uint64_t lastLap{stopWatch.lap()};

        // Hyperparameter optimisation loop.

        this->initializePerFoldTestLosses();

        while (m_CurrentRound < m_NumberRounds) {

            LOG_TRACE(<< "Optimisation round = " << m_CurrentRound + 1);
            m_Instrumentation->iteration(m_CurrentRound + 1);

            this->recordHyperparameters();

            TMeanVarAccumulator lossMoments;
            std::size_t maximumNumberTrees;
            std::tie(lossMoments, maximumNumberTrees) = this->crossValidateForest(frame);

            this->captureBestHyperparameters(lossMoments, maximumNumberTrees);

            if (this->selectNextHyperparameters(lossMoments, *m_BayesianOptimization) == false) {
                LOG_WARN(<< "Hyperparameter selection failed: exiting loop early");
                break;
            }

            std::int64_t memoryUsage(this->memoryUsage());
            m_Instrumentation->updateMemoryUsage(memoryUsage - lastMemoryUsage);
            lastMemoryUsage = memoryUsage;

            // Store the training state after each hyperparameter search step.
            m_CurrentRound += 1;
            LOG_TRACE(<< "Round " << m_CurrentRound << " state recording started");
            this->recordState(recordTrainStateCallback);
            LOG_TRACE(<< "Round " << m_CurrentRound << " state recording finished");

            std::uint64_t currentLap{stopWatch.lap()};
            std::uint64_t delta{currentLap - lastLap};
            m_Instrumentation->iterationTime(delta);

            timeAccumulator.add(static_cast<double>(delta));
            lastLap = currentLap;
            m_Instrumentation->flush(HYPERPARAMETER_OPTIMIZATION_ROUND +
                                     std::to_string(m_CurrentRound));
        }

        LOG_TRACE(<< "Test loss = " << m_BestForestTestLoss);

        this->restoreBestHyperparameters();
        this->startProgressMonitoringFinalTrain();
        std::tie(m_BestForest, std::ignore, std::ignore) = this->trainForest(
            frame, allTrainingRowsMask, allTrainingRowsMask, m_TrainingProgress);

        this->recordState(recordTrainStateCallback);
        m_Instrumentation->iteration(m_CurrentRound);
        m_Instrumentation->flush(TRAIN_FINAL_FOREST);

        timeAccumulator.add(static_cast<double>(stopWatch.stop()));

        LOG_TRACE(<< "Training finished after " << m_CurrentRound << " iterations. "
                  << "Time per iteration in ms mean: "
                  << CBasicStatistics::mean(timeAccumulator) << " std. dev:  "
                  << std::sqrt(CBasicStatistics::variance(timeAccumulator)));

        core::CProgramCounters::counter(counter_t::E_DFTPMTrainedForestNumberTrees) =
            m_BestForest.size();
    }

    this->computeClassificationWeights(frame);
    this->initializeTreeShap(frame);

    // Force progress to one because we can have early exit from loop skip altogether.
    m_Instrumentation->updateProgress(1.0);
    m_Instrumentation->updateMemoryUsage(
        static_cast<std::int64_t>(this->memoryUsage()) - lastMemoryUsage);
}

void CBoostedTreeImpl::recordState(const TTrainingStateCallback& recordTrainState) const {
    recordTrainState([this](core::CStatePersistInserter& inserter) {
        this->acceptPersistInserter(inserter);
    });
}

void CBoostedTreeImpl::predict(core::CDataFrame& frame) const {
    if (m_BestForestTestLoss == INF) {
        HANDLE_FATAL(<< "Internal error: no model available for prediction. "
                     << "Please report this problem.")
        return;
    }
    bool successful;
    std::tie(std::ignore, successful) = frame.writeColumns(
        m_NumberThreads, 0, frame.numberRows(), [&](TRowItr beginRows, TRowItr endRows) {
            std::size_t numberLossParameters{m_Loss->numberParameters()};
            for (auto row = beginRows; row != endRows; ++row) {
                auto prediction = readPrediction(*row, m_ExtraColumns, numberLossParameters);
                prediction = predictRow(m_Encoder->encode(*row), m_BestForest);
            }
        });
    if (successful == false) {
        HANDLE_FATAL(<< "Internal error: failed model inference. "
                     << "Please report this problem.")
    }
}

std::size_t CBoostedTreeImpl::estimateMemoryUsage(std::size_t numberRows,
                                                  std::size_t numberColumns) const {
    // The maximum tree size is defined is the maximum number of leaves minus one.
    // A binary tree with n + 1 leaves has 2n + 1 nodes in total.
    std::size_t maximumNumberLeaves{this->maximumTreeSize(numberRows) + 1};
    std::size_t maximumNumberNodes{2 * maximumNumberLeaves - 1};
    std::size_t maximumNumberFeatures{std::min(numberColumns - 1, numberRows / m_RowsPerFeature)};
    std::size_t forestMemoryUsage{
        m_MaximumNumberTrees *
        (sizeof(TNodeVec) + maximumNumberNodes * CBoostedTreeNode::estimateMemoryUsage(
                                                     m_Loss->numberParameters()))};
    std::size_t foldRoundLossMemoryUsage{m_NumberFolds * m_NumberRounds *
                                         sizeof(TOptionalDouble)};
    std::size_t hyperparametersMemoryUsage{numberColumns * sizeof(double)};
    // The leaves' row masks memory is accounted for here because it's proportional
    // to the log2(number of nodes). The compressed bit vector representation uses
    // roughly log2(E[run length]) / E[run length] bytes per bit. As we grow the
    // tree we partition the data and so the average run length (sequential unmasked
    // values) is just equal to the number of leaves. Putting this together, if there
    // are n rows and m leaves each leaf will use n * log2(m) / m and so their total
    // memory will be n * log2(m). In practice, we don't get the optimal compression,
    // a reasonable margin is a factor of 4.
    std::size_t rowMaskMemoryUsage{4 * numberRows *
                                   static_cast<std::size_t>(std::ceil(std::log2(
                                       static_cast<double>(maximumNumberLeaves))))};
    // We only maintain statistics for leaves we know we may possibly split this
    // halves the peak number of statistics we maintain.
    std::size_t leafNodeStatisticsMemoryUsage{
        rowMaskMemoryUsage + maximumNumberLeaves *
                                 CBoostedTreeLeafNodeStatistics::estimateMemoryUsage(
                                     maximumNumberFeatures, m_NumberSplitsPerFeature,
                                     m_Loss->numberParameters()) /
                                 2};
    std::size_t dataTypeMemoryUsage{maximumNumberFeatures * sizeof(CDataFrameUtils::SDataType)};
    std::size_t featureSampleProbabilities{maximumNumberFeatures * sizeof(double)};
    // Assuming either many or few missing rows, we get good compression of the bit
    // vector. Specifically, we'll assume the average run length is 256 for which
    // we get a constant 4 * 8 / 256.
    std::size_t missingFeatureMaskMemoryUsage{32 * numberColumns * numberRows / 256};
    std::size_t trainTestMaskMemoryUsage{
        2 * static_cast<std::size_t>(std::ceil(std::log2(static_cast<double>(m_NumberFolds)))) *
        numberRows};
    std::size_t bayesianOptimisationMemoryUsage{CBayesianOptimisation::estimateMemoryUsage(
        this->numberHyperparametersToTune(), m_NumberRounds)};
    std::size_t worstCaseMemoryUsage{
        sizeof(*this) + forestMemoryUsage + foldRoundLossMemoryUsage +
        hyperparametersMemoryUsage + leafNodeStatisticsMemoryUsage +
        dataTypeMemoryUsage + featureSampleProbabilities + missingFeatureMaskMemoryUsage +
        trainTestMaskMemoryUsage + bayesianOptimisationMemoryUsage};
    // We compute the correction coefficient as a sigmoid function: ca. 1.0 until
    // 10mb, 16.0 after 1000mb to this end we need to shift and scale using the
    // magic numbers below.
    double correctionCoefficient{
        CTools::logisticFunction(
            static_cast<double>(worstCaseMemoryUsage) / (1024 * 1024), 10, 550) *
            15 +
        1};
    return static_cast<std::size_t>(static_cast<double>(worstCaseMemoryUsage) / correctionCoefficient);
}

bool CBoostedTreeImpl::canTrain() const {
    return std::accumulate(m_FeatureSampleProbabilities.begin(),
                           m_FeatureSampleProbabilities.end(), 0.0) > 0.0;
}

core::CPackedBitVector CBoostedTreeImpl::allTrainingRowsMask() const {
    return ~m_MissingFeatureRowMasks[m_DependentVariable];
}

CBoostedTreeImpl::TDoubleDoublePr
CBoostedTreeImpl::gainAndCurvatureAtPercentile(double percentile,
                                               const TNodeVecVec& forest) const {

    TDoubleVec gains;
    TDoubleVec curvatures;

    for (const auto& tree : forest) {
        for (const auto& node : tree) {
            if (node.isLeaf() == false) {
                gains.push_back(node.gain());
                curvatures.push_back(node.curvature());
            }
        }
    }

    if (gains.size() == 0) {
        return {0.0, 0.0};
    }

    std::size_t index{std::min(
        static_cast<std::size_t>(percentile * static_cast<double>(gains.size()) / 100.0 + 0.5),
        gains.size() - 1)};
    std::nth_element(gains.begin(), gains.begin() + index, gains.end());
    std::nth_element(curvatures.begin(), curvatures.begin() + index, curvatures.end());

    return {gains[index], curvatures[index]};
}

void CBoostedTreeImpl::initializePerFoldTestLosses() {
    m_FoldRoundTestLosses.resize(m_NumberFolds);
    for (auto& losses : m_FoldRoundTestLosses) {
        losses.resize(m_NumberRounds);
    }
}

void CBoostedTreeImpl::computeClassificationWeights(const core::CDataFrame& frame) {

    using TFloatStorageVec = std::vector<CFloatStorage>;

    if (m_Loss->type() == E_BinaryClassification || m_Loss->type() == E_MulticlassClassification) {

        std::size_t numberClasses{
            m_Loss->type() == E_BinaryClassification ? 2 : m_Loss->numberParameters()};
        TFloatStorageVec storage(2);

        switch (m_ClassAssignmentObjective) {
        case CBoostedTree::E_Accuracy:
            m_ClassificationWeights = TVector::Ones(numberClasses);
            break;
        case CBoostedTree::E_MinimumRecall:
            m_ClassificationWeights = CDataFrameUtils::maximumMinimumRecallClassWeights(
                m_NumberThreads, frame, this->allTrainingRowsMask(),
                numberClasses, m_DependentVariable,
                [storage, numberClasses, this](const TRowRef& row) mutable {
                    if (m_Loss->type() == E_BinaryClassification) {
                        // We predict the log-odds but this is expected to return
                        // the log of the predicted class probabilities.
                        TMemoryMappedFloatVector result{&storage[0], 2};
                        result.array() =
                            m_Loss
                                ->transform(readPrediction(row, m_ExtraColumns, numberClasses))
                                .array()
                                .log();
                        return result;
                    }
                    return readPrediction(row, m_ExtraColumns, numberClasses);
                });
            break;
        }
    }
}

void CBoostedTreeImpl::initializeTreeShap(const core::CDataFrame& frame) {
    // Populate number samples reaching each node.
    CTreeShapFeatureImportance::computeNumberSamples(m_NumberThreads, frame,
                                                     *m_Encoder, m_BestForest);

    if (m_NumberTopShapValues > 0) {
        // Create the SHAP calculator.
        m_TreeShap = std::make_unique<CTreeShapFeatureImportance>(
            m_NumberThreads, frame, *m_Encoder, m_BestForest, m_NumberTopShapValues);
    } else {
        // TODO these are not currently written into the inference model
        // but they would be nice to expose since they provide good insight
        // into how the splits affect the target variable.
        // Set internal node values anyway.
        //CTreeShapFeatureImportance::computeInternalNodeValues(m_BestForest);
    }
}

CBoostedTreeImpl::TMeanVarAccumulatorSizePr
CBoostedTreeImpl::crossValidateForest(core::CDataFrame& frame) {

    // We want to ensure we evaluate on equal proportions for each fold.
    TSizeVec folds(m_NumberFolds);
    std::iota(folds.begin(), folds.end(), 0);
    CSampling::random_shuffle(m_Rng, folds.begin(), folds.end());

    auto stopCrossValidationEarly = [&](TMeanVarAccumulator testLossMoments) {
        // Always train on at least one fold and every fold for the first
        // "number folds" rounds. Exit cross-validation early if it's clear
        // that the test error is not close to the minimum test error. We use
        // the estimated test error for each remaining fold at two standard
        // deviations below the mean for this.
        if (m_StopCrossValidationEarly && m_CurrentRound >= m_NumberFolds &&
            folds.size() < m_NumberFolds) {
            for (const auto& testLoss : this->estimateMissingTestLosses(folds)) {
                testLossMoments.add(
                    CBasicStatistics::mean(testLoss) -
                    2.0 * std::sqrt(CBasicStatistics::maximumLikelihoodVariance(testLoss)));
            }
            return CBasicStatistics::mean(testLossMoments) > this->minimumTestLoss();
        }
        return false;
    };

    TMeanVarAccumulator lossMoments;
    TDoubleVec numberTrees;
    numberTrees.reserve(m_NumberFolds);

    while (folds.size() > 0 && stopCrossValidationEarly(lossMoments) == false) {
        std::size_t fold{folds.back()};
        folds.pop_back();
        TNodeVecVec forest;
        double loss;
        TDoubleVec lossValues;
        std::tie(forest, loss, lossValues) = this->trainForest(
            frame, m_TrainingRowMasks[fold], m_TestingRowMasks[fold], m_TrainingProgress);
        LOG_TRACE(<< "fold = " << fold << " forest size = " << forest.size()
                  << " test set loss = " << loss);
        lossMoments.add(loss);
        m_FoldRoundTestLosses[fold][m_CurrentRound] = loss;
        numberTrees.push_back(static_cast<double>(forest.size()));
        m_Instrumentation->lossValues(fold, std::move(lossValues));
    }
    m_TrainingProgress.increment(m_MaximumNumberTrees * folds.size());
    LOG_TRACE(<< "skipped " << folds.size() << " folds");

    std::sort(numberTrees.begin(), numberTrees.end());
    std::size_t medianNumberTrees{
        static_cast<std::size_t>(CBasicStatistics::median(numberTrees))};
    lossMoments = this->correctTestLossMoments(std::move(folds), lossMoments);
    LOG_TRACE(<< "test mean loss = " << CBasicStatistics::mean(lossMoments)
              << ", sigma = " << std::sqrt(CBasicStatistics::mean(lossMoments)));

    return {lossMoments, medianNumberTrees};
}

CBoostedTreeImpl::TNodeVec CBoostedTreeImpl::initializePredictionsAndLossDerivatives(
    core::CDataFrame& frame,
    const core::CPackedBitVector& trainingRowMask,
    const core::CPackedBitVector& testingRowMask) const {

    core::CPackedBitVector updateRowMask{trainingRowMask | testingRowMask};
    frame.writeColumns(
        m_NumberThreads, 0, frame.numberRows(),
        [this](TRowItr beginRows, TRowItr endRows) {
            std::size_t numberLossParameters{m_Loss->numberParameters()};
            for (auto row = beginRows; row != endRows; ++row) {
                zeroPrediction(*row, m_ExtraColumns, numberLossParameters);
                zeroLossGradient(*row, m_ExtraColumns, numberLossParameters);
                zeroLossCurvature(*row, m_ExtraColumns, numberLossParameters);
            }
        },
        &updateRowMask);

    // At the start we will centre the data w.r.t. the given loss function.
    TNodeVec tree{CBoostedTreeNode{m_Loss->numberParameters()}};
    this->refreshPredictionsAndLossDerivatives(frame, trainingRowMask, testingRowMask,
                                               1.0 /*eta*/, 0.0 /*lambda*/, tree);

    return tree;
}

CBoostedTreeImpl::TNodeVecVecDoubleDoubleVecTuple
CBoostedTreeImpl::trainForest(core::CDataFrame& frame,
                              const core::CPackedBitVector& trainingRowMask,
                              const core::CPackedBitVector& testingRowMask,
                              core::CLoopProgress& trainingProgress) const {

    LOG_TRACE(<< "Training one forest...");

    std::size_t maximumTreeSize{this->maximumTreeSize(trainingRowMask)};

    TNodeVecVec forest{this->initializePredictionsAndLossDerivatives(
        frame, trainingRowMask, testingRowMask)};
    forest.reserve(m_MaximumNumberTrees);

    CScopeRecordMemoryUsage scopeMemoryUsage{forest, m_Instrumentation->memoryUsageCallback()};

    // For each iteration:
    //  1. Compute weighted quantiles for features F
    //  2. Periodically compute candidate split set S from quantiles of F
    //  3. Build one tree on (F, S)
    //  4. Update predictions and loss derivatives

    double eta{m_Eta};

    // Computing feature quantiles is surprisingly runtime expensive and there may
    // be mileage in seeing if we can make the sketch more efficient. However, we
    // should easily be able to build multiple trees on the same set of candidate
    // splits and decrease the loss function since the space of candidate trees for
    // a fixed set of candidate splits is very large. Furthermore, we use a greedy
    // heuristic to search this space and so don't expect to find the tree to add
    // which minimises total loss. As a result, we choose to amortise the cost of
    // computing feature quantiles by only refreshing candidate splits periodically.
    std::size_t nextTreeCountToRefreshSplits{
        forest.size() + static_cast<std::size_t>(std::max(0.5 / eta, 1.0))};

    auto downsampledRowMask = this->downsample(trainingRowMask);
    scopeMemoryUsage.add(downsampledRowMask);
    auto candidateSplits = this->candidateSplits(frame, downsampledRowMask);
    scopeMemoryUsage.add(candidateSplits);

    std::size_t retries{0};

    TDoubleVec losses;
    losses.reserve(m_MaximumNumberTrees);
    CTrainForestStoppingCondition stoppingCondition{m_MaximumNumberTrees};
    TWorkspace workspace;

    do {
        auto tree = this->trainTree(frame, downsampledRowMask, candidateSplits,
                                    maximumTreeSize, workspace);

        retries = tree.size() == 1 ? retries + 1 : 0;

        if (retries == m_MaximumAttemptsToAddTree) {
            break;
        }

        if (tree.size() > 1) {
            scopeMemoryUsage.add(tree);
            this->refreshPredictionsAndLossDerivatives(
                frame, trainingRowMask, testingRowMask, eta,
                m_Regularization.leafWeightPenaltyMultiplier(), tree);
            forest.push_back(std::move(tree));
            eta = std::min(1.0, m_EtaGrowthRatePerTree * eta);
            retries = 0;
            trainingProgress.increment();
        } else {
            // Refresh splits in case it allows us to find tree which can reduce loss.
            candidateSplits = this->candidateSplits(frame, downsampledRowMask);
            nextTreeCountToRefreshSplits += static_cast<std::size_t>(
                std::max(0.5 / eta, MINIMUM_SPLIT_REFRESH_INTERVAL));
        }

        downsampledRowMask = this->downsample(trainingRowMask);

        if (forest.size() == nextTreeCountToRefreshSplits) {
            candidateSplits = this->candidateSplits(frame, downsampledRowMask);
            nextTreeCountToRefreshSplits += static_cast<std::size_t>(
                std::max(0.5 / eta, MINIMUM_SPLIT_REFRESH_INTERVAL));
        }
    } while (stoppingCondition.shouldStop(forest.size(), [&]() {
        double loss{this->meanLoss(frame, testingRowMask)};
        losses.push_back(loss);
        return loss;
    }) == false);

    LOG_TRACE(<< "Stopped at " << forest.size() - 1 << "/" << m_MaximumNumberTrees);

    trainingProgress.increment(std::max(m_MaximumNumberTrees, forest.size()) -
                               forest.size());

    forest.resize(stoppingCondition.bestSize());

    LOG_TRACE(<< "Trained one forest");

    return {forest, stoppingCondition.bestLoss(), std::move(losses)};
}

core::CPackedBitVector
CBoostedTreeImpl::downsample(const core::CPackedBitVector& trainingRowMask) const {
    // We compute a stochastic version of the candidate splits, gradients and
    // curvatures for each tree we train. The sampling scheme should minimize
    // the correlation with previous trees for fixed sample size so randomly
    // sampling without replacement is appropriate.
    core::CPackedBitVector result;
    do {
        result = core::CPackedBitVector{};
        for (auto i = trainingRowMask.beginOneBits();
             i != trainingRowMask.endOneBits(); ++i) {
            if (CSampling::uniformSample(m_Rng, 0.0, 1.0) < m_DownsampleFactor) {
                result.extend(false, *i - result.size());
                result.extend(true);
            }
        }
    } while (result.manhattan() == 0.0);
    result.extend(false, trainingRowMask.size() - result.size());
    return result;
}

CBoostedTreeImpl::TImmutableRadixSetVec
CBoostedTreeImpl::candidateSplits(const core::CDataFrame& frame,
                                  const core::CPackedBitVector& trainingRowMask) const {

    TSizeVec features;
    this->candidateRegressorFeatures(m_FeatureSampleProbabilities, features);
    LOG_TRACE(<< "candidate features = " << core::CContainerPrinter::print(features));

    TSizeVec binaryFeatures(features);
    binaryFeatures.erase(std::remove_if(binaryFeatures.begin(), binaryFeatures.end(),
                                        [this](std::size_t index) {
                                            return m_Encoder->isBinary(index) == false;
                                        }),
                         binaryFeatures.end());
    CSetTools::inplace_set_difference(features, binaryFeatures.begin(),
                                      binaryFeatures.end());
    LOG_TRACE(<< "binary features = " << core::CContainerPrinter::print(binaryFeatures)
              << " other features = " << core::CContainerPrinter::print(features));

    auto featureQuantiles =
        CDataFrameUtils::columnQuantiles(
            m_NumberThreads, frame, trainingRowMask, features,
            CFastQuantileSketch{CFastQuantileSketch::E_Linear,
                                std::max(m_NumberSplitsPerFeature, std::size_t{50}), m_Rng},
            m_Encoder.get(),
            [this](const TRowRef& row) {
                std::size_t numberLossParameters{m_Loss->numberParameters()};
                return trace(numberLossParameters,
                             readLossCurvature(row, m_ExtraColumns, numberLossParameters));
            })
            .first;

    TImmutableRadixSetVec candidateSplits(this->numberFeatures());

    for (auto i : binaryFeatures) {
        candidateSplits[i] = core::CImmutableRadixSet<double>{0.5};
        LOG_TRACE(<< "feature '" << i << "' splits = " << candidateSplits[i].print());
    }
    for (std::size_t i = 0; i < features.size(); ++i) {

        TDoubleVec featureSplits;
        featureSplits.reserve(m_NumberSplitsPerFeature - 1);

        for (std::size_t j = 1; j < m_NumberSplitsPerFeature; ++j) {
            double rank{100.0 * static_cast<double>(j) / static_cast<double>(m_NumberSplitsPerFeature) +
                        CSampling::uniformSample(m_Rng, -0.1, 0.1)};
            double q;
            if (featureQuantiles[i].quantile(rank, q)) {
                featureSplits.push_back(q);
            } else {
                LOG_WARN(<< "Failed to compute quantile " << rank << ": ignoring split");
            }
        }

        const auto& dataType = m_FeatureDataTypes[features[i]];

        if (dataType.s_IsInteger) {
            // The key point here is that we know that if two distinct splits fall
            // between two consecutive integers they must produce identical partitions
            // of the data and so always have the same loss. We only need to retain
            // one such split for training. We achieve this by snapping to the midpoint
            // and subsquently deduplicating.
            std::for_each(featureSplits.begin(), featureSplits.end(),
                          [](double& split) { split = std::floor(split) + 0.5; });
        }
        featureSplits.erase(std::unique(featureSplits.begin(), featureSplits.end()),
                            featureSplits.end());
        featureSplits.erase(std::remove_if(featureSplits.begin(), featureSplits.end(),
                                           [&dataType](double split) {
                                               return split < dataType.s_Min ||
                                                      split > dataType.s_Max;
                                           }),
                            featureSplits.end());
        candidateSplits[features[i]] =
            core::CImmutableRadixSet<double>{std::move(featureSplits)};

        LOG_TRACE(<< "feature '" << features[i]
                  << "' splits = " << candidateSplits[features[i]].print());
    }

    return candidateSplits;
}

CBoostedTreeImpl::TNodeVec
CBoostedTreeImpl::trainTree(core::CDataFrame& frame,
                            const core::CPackedBitVector& trainingRowMask,
                            const TImmutableRadixSetVec& candidateSplits,
                            const std::size_t maximumTreeSize,
                            TWorkspace& workspace) const {

    LOG_TRACE(<< "Training one tree...");

    using TLeafNodeStatisticsPtr = CBoostedTreeLeafNodeStatistics::TPtr;
    using TLeafNodeStatisticsPtrQueue = boost::circular_buffer<TLeafNodeStatisticsPtr>;

    workspace.reinitialize(m_NumberThreads, candidateSplits, m_Loss->numberParameters());

    TNodeVec tree(1);
    tree.reserve(2 * maximumTreeSize + 1);

    // Sampling transforms the probabilities. We use a placeholder outside
    // the loop adding nodes so we only allocate the vector once.
    TDoubleVec featureSampleProbabilities{m_FeatureSampleProbabilities};
    TSizeVec featureBag;
    this->featureBag(featureSampleProbabilities, featureBag);

    TLeafNodeStatisticsPtrQueue leaves(maximumTreeSize / 2 + 3);
    leaves.push_back(std::make_shared<CBoostedTreeLeafNodeStatistics>(
        0 /*root*/, m_ExtraColumns, m_Loss->numberParameters(), m_NumberThreads,
        frame, *m_Encoder, m_Regularization, candidateSplits, featureBag,
        0 /*depth*/, trainingRowMask, workspace));

    // We update local variables because the callback can be expensive if it
    // requires accessing atomics.
    struct SMemoryStats {
        std::int64_t s_Current = 0;
        std::int64_t s_Max = 0;
    } memory;
    TMemoryUsageCallback localRecordMemoryUsage{[&](std::int64_t delta) {
        memory.s_Current += delta;
        memory.s_Max = std::max(memory.s_Max, memory.s_Current);
    }};
    CScopeRecordMemoryUsage scopeMemoryUsage{leaves, std::move(localRecordMemoryUsage)};
    scopeMemoryUsage.add(workspace);

    // For each iteration we:
    //   1. Find the leaf with the greatest decrease in loss
    //   2. If no split (significantly) reduced the loss we terminate
    //   3. Otherwise we split that leaf

    double totalGain{0.0};

    COrderings::SLess less;

    for (std::size_t i = 0; i < maximumTreeSize; ++i) {

        if (leaves.empty()) {
            break;
        }

        auto leaf = leaves.back();
        leaves.pop_back();

        scopeMemoryUsage.remove(leaf);

        if (leaf->gain() < MINIMUM_RELATIVE_GAIN_PER_SPLIT * totalGain) {
            break;
        }

        totalGain += leaf->gain();
        workspace.minimumGain(MINIMUM_RELATIVE_GAIN_PER_SPLIT * totalGain);
        LOG_TRACE(<< "splitting " << leaf->id() << " leaf gain = " << leaf->gain()
                  << " total gain = " << totalGain);

        std::size_t splitFeature;
        double splitValue;
        std::tie(splitFeature, splitValue) = leaf->bestSplit();

        bool assignMissingToLeft{leaf->assignMissingToLeft()};

        std::size_t leftChildId, rightChildId;
        std::tie(leftChildId, rightChildId) =
            tree[leaf->id()].split(splitFeature, splitValue, assignMissingToLeft,
                                   leaf->gain(), leaf->curvature(), tree);

        featureSampleProbabilities = m_FeatureSampleProbabilities;
        this->featureBag(featureSampleProbabilities, featureBag);

        TLeafNodeStatisticsPtr leftChild;
        TLeafNodeStatisticsPtr rightChild;
        std::tie(leftChild, rightChild) = leaf->split(
            leftChildId, rightChildId, m_NumberThreads, frame, *m_Encoder,
            m_Regularization, featureBag, tree[leaf->id()], workspace);

        if (less(rightChild, leftChild)) {
            std::swap(leftChild, rightChild);
        }

        std::size_t n{leaves.size()};
        if (leftChild->gain() >= MINIMUM_RELATIVE_GAIN_PER_SPLIT * totalGain) {
            scopeMemoryUsage.add(leftChild);
            leaves.push_back(std::move(leftChild));
        }
        if (rightChild->gain() >= MINIMUM_RELATIVE_GAIN_PER_SPLIT * totalGain) {
            scopeMemoryUsage.add(rightChild);
            leaves.push_back(std::move(rightChild));
        }
        std::inplace_merge(leaves.begin(), leaves.begin() + n, leaves.end(), less);

        // Drop any leaves which can't possibly be split.
        while (leaves.size() + i + 1 > maximumTreeSize) {
            scopeMemoryUsage.remove(leaves.front());
            workspace.minimumGain(leaves.front()->gain());
            leaves.pop_front();
        }
    }

    tree.shrink_to_fit();

    // Flush the maximum memory used by the leaf statistics to the callback.
    m_Instrumentation->updateMemoryUsage(memory.s_Max);
    m_Instrumentation->updateMemoryUsage(-memory.s_Max);

    LOG_TRACE(<< "Trained one tree. # nodes = " << tree.size());

    return tree;
}

double CBoostedTreeImpl::minimumTestLoss() const {
    using TMinAccumulator = CBasicStatistics::SMin<double>::TAccumulator;
    TMinAccumulator minimumTestLoss;
    for (std::size_t round = 0; round < m_CurrentRound - 1; ++round) {
        TMeanVarAccumulator roundLossMoments;
        for (std::size_t fold = 0; fold < m_NumberFolds; ++fold) {
            if (m_FoldRoundTestLosses[fold][round] != boost::none) {
                roundLossMoments.add(*m_FoldRoundTestLosses[fold][round]);
            }
        }
        if (static_cast<std::size_t>(CBasicStatistics::count(roundLossMoments)) == m_NumberFolds) {
            minimumTestLoss.add(CBasicStatistics::mean(roundLossMoments));
        }
    }
    return minimumTestLoss[0];
}

TMeanVarAccumulator
CBoostedTreeImpl::correctTestLossMoments(const TSizeVec& missing,
                                         TMeanVarAccumulator lossMoments) const {
    if (missing.empty()) {
        return lossMoments;
    }
    for (const auto& loss : this->estimateMissingTestLosses(missing)) {
        lossMoments += loss;
    }
    return lossMoments;
}

CBoostedTreeImpl::TMeanVarAccumulatorVec
CBoostedTreeImpl::estimateMissingTestLosses(const TSizeVec& missing) const {

    // We have a subset of folds for which we've computed test loss. We want to
    // estimate the test loss we'll see for the remaining folds to decide if it
    // is worthwhile to continue training with these parameters and to correct
    // the loss value supplied to Bayesian Optimisation to account for the folds
    // we haven't trained on. We tackle this problem as follows:
    //   1. Find all previous rounds R which share at least one fold with the
    //      current round, i.e. one fold for which we've computed the actual
    //      loss for the current round parameters.
    //   2. For each fold f_i for which we haven't estimated the loss in the
    //      current round fit an OLS model m_i to R to predict the loss of f_i.
    //   3. Compute the predicted value for the test loss on each f_i given
    //      the test losses we've computed so far the current round using m_i.
    //   4. Estimate the uncertainty from the variance of the residuals from
    //      fitting the model m_i to R.
    //
    // The feature vector we use is defined as:
    //
    //   |   calculated fold error 1  |
    //   |   calculated fold error 2  |
    //   |             ...            |
    //   | 1{fold error 1 is present} |
    //   | 1{fold error 2 is present} |
    //   |             ...            |
    //
    // where the indices range over the folds for which we have errors in the
    // current round.

    TSizeVec present(m_NumberFolds);
    std::iota(present.begin(), present.end(), 0);
    TSizeVec ordered{missing};
    std::sort(ordered.begin(), ordered.end());
    CSetTools::inplace_set_difference(present, ordered.begin(), ordered.end());
    LOG_TRACE(<< "present = " << core::CContainerPrinter::print(present));

    // Get the current round feature vector. Fixed so computed outside the loop.
    TVector x(2 * present.size());
    for (std::size_t col = 0; col < present.size(); ++col) {
        x(col) = *m_FoldRoundTestLosses[present[col]][m_CurrentRound];
        x(present.size() + col) = 0.0;
    }

    TMeanVarAccumulatorVec predictedTestLosses;
    predictedTestLosses.reserve(missing.size());

    for (std::size_t target : missing) {
        // Extract the training mask.
        TSizeVec trainingMask;
        trainingMask.reserve(m_CurrentRound);
        for (std::size_t round = 0; round < m_CurrentRound; ++round) {
            if (m_FoldRoundTestLosses[target][round] &&
                std::find_if(present.begin(), present.end(), [&](std::size_t fold) {
                    return m_FoldRoundTestLosses[fold][round];
                }) != present.end()) {
                trainingMask.push_back(round);
            }
        }

        // Fit the OLS regression.
        CDenseMatrix<double> A(trainingMask.size(), 2 * present.size());
        TVector b(trainingMask.size());
        for (std::size_t row = 0; row < trainingMask.size(); ++row) {
            for (std::size_t col = 0; col < present.size(); ++col) {
                if (m_FoldRoundTestLosses[present[col]][trainingMask[row]]) {
                    A(row, col) = *m_FoldRoundTestLosses[present[col]][trainingMask[row]];
                    A(row, present.size() + col) = 0.0;
                } else {
                    A(row, col) = 0.0;
                    A(row, present.size() + col) = 1.0;
                }
            }
            b(row) = *m_FoldRoundTestLosses[target][trainingMask[row]];
        }
        TVector params{A.colPivHouseholderQr().solve(b)};

        TMeanVarAccumulator residualMoments;
        for (int row = 0; row < A.rows(); ++row) {
            residualMoments.add(b(row) - A.row(row) * params);
        }

        double predictedTestLoss{params.transpose() * x};
        double predictedTestLossVariance{
            CBasicStatistics::maximumLikelihoodVariance(residualMoments)};
        LOG_TRACE(<< "prediction(x = " << x.transpose() << ", fold = " << target
                  << ") = (mean = " << predictedTestLoss
                  << ", variance = " << predictedTestLossVariance << ")");

        predictedTestLosses.push_back(CBasicStatistics::momentsAccumulator(
            1.0, predictedTestLoss, predictedTestLossVariance));
    }

    return predictedTestLosses;
}

std::size_t CBoostedTreeImpl::numberFeatures() const {
    return m_Encoder->numberEncodedColumns();
}

std::size_t CBoostedTreeImpl::featureBagSize() const {
    return static_cast<std::size_t>(std::max(
        std::ceil(m_FeatureBagFraction * static_cast<double>(this->numberFeatures())), 1.0));
}

void CBoostedTreeImpl::featureBag(TDoubleVec& probabilities, TSizeVec& bag) const {

    std::size_t size{this->featureBagSize()};

    this->candidateRegressorFeatures(probabilities, bag);
    if (size >= bag.size()) {
        return;
    }

    CSampling::categoricalSampleWithoutReplacement(m_Rng, probabilities, size, bag);
    std::sort(bag.begin(), bag.end());
}

void CBoostedTreeImpl::candidateRegressorFeatures(const TDoubleVec& probabilities,
                                                  TSizeVec& features) const {
    features.clear();
    features.reserve(probabilities.size());
    for (std::size_t i = 0; i < probabilities.size(); ++i) {
        if (probabilities[i] > 0.0) {
            features.push_back(i);
        }
    }
}

const CBoostedTreeNode& CBoostedTreeImpl::root(const TNodeVec& tree) {
    return tree[0];
}

void CBoostedTreeImpl::refreshPredictionsAndLossDerivatives(core::CDataFrame& frame,
                                                            const core::CPackedBitVector& trainingRowMask,
                                                            const core::CPackedBitVector& testingRowMask,
                                                            double eta,
                                                            double lambda,
                                                            TNodeVec& tree) const {

    using TArgMinLossVec = std::vector<CArgMinLoss>;

    TArgMinLossVec leafValues(tree.size(), m_Loss->minimizer(lambda, m_Rng));
    auto nextPass = [&] {
        bool done{true};
        for (const auto& value : leafValues) {
            done &= (value.nextPass() == false);
        }
        return done == false;
    };

    do {
        auto result = frame.readRows(
            m_NumberThreads, 0, frame.numberRows(),
            core::bindRetrievableState(
                [&](TArgMinLossVec& leafValues_, TRowItr beginRows, TRowItr endRows) {
                    std::size_t numberLossParameters{m_Loss->numberParameters()};
                    const auto& rootNode = root(tree);
                    for (auto row_ = beginRows; row_ != endRows; ++row_) {
                        auto row = *row_;
                        auto prediction = readPrediction(row, m_ExtraColumns,
                                                         numberLossParameters);
                        double actual{readActual(row, m_DependentVariable)};
                        double weight{readExampleWeight(row, m_ExtraColumns)};
                        leafValues_[rootNode.leafIndex(m_Encoder->encode(row), tree)]
                            .add(prediction, actual, weight);
                    }
                },
                std::move(leafValues)),
            &trainingRowMask);

        leafValues = std::move(result.first[0].s_FunctionState);
        for (std::size_t i = 1; i < result.first.size(); ++i) {
            for (std::size_t j = 0; j < leafValues.size(); ++j) {
                leafValues[j].merge(result.first[i].s_FunctionState[j]);
            }
        }
    } while (nextPass());

    for (std::size_t i = 0; i < tree.size(); ++i) {
        if (tree[i].isLeaf()) {
            tree[i].value(eta * leafValues[i].value());
        }
    }

    LOG_TRACE(<< "tree =\n" << root(tree).print(tree));

    core::CPackedBitVector updateRowMask{trainingRowMask | testingRowMask};
    frame.writeColumns(
        m_NumberThreads, 0, frame.numberRows(),
        [&](TRowItr beginRows, TRowItr endRows) {
            std::size_t numberLossParameters{m_Loss->numberParameters()};
            const auto& rootNode = root(tree);
            for (auto row_ = beginRows; row_ != endRows; ++row_) {
                auto row = *row_;
                auto prediction = readPrediction(row, m_ExtraColumns, numberLossParameters);
                double actual{readActual(row, m_DependentVariable)};
                double weight{readExampleWeight(row, m_ExtraColumns)};
                prediction += rootNode.value(m_Encoder->encode(row), tree);
                writeLossGradient(row, m_ExtraColumns, *m_Loss, prediction, actual, weight);
                writeLossCurvature(row, m_ExtraColumns, *m_Loss, prediction, actual, weight);
            }
        },
        &updateRowMask);
}

double CBoostedTreeImpl::meanLoss(const core::CDataFrame& frame,
                                  const core::CPackedBitVector& rowMask) const {

    auto results = frame.readRows(
        m_NumberThreads, 0, frame.numberRows(),
        core::bindRetrievableState(
            [&](TMeanAccumulator& loss, TRowItr beginRows, TRowItr endRows) {
                std::size_t numberLossParameters{m_Loss->numberParameters()};
                for (auto row = beginRows; row != endRows; ++row) {
                    auto prediction = readPrediction(*row, m_ExtraColumns, numberLossParameters);
                    double actual{readActual(*row, m_DependentVariable)};
                    loss.add(m_Loss->value(prediction, actual));
                }
            },
            TMeanAccumulator{}),
        &rowMask);

    TMeanAccumulator loss;
    for (const auto& result : results.first) {
        loss += result.s_FunctionState;
    }

    LOG_TRACE(<< "mean loss = " << CBasicStatistics::mean(loss));

    return CBasicStatistics::mean(loss);
}

CBoostedTreeNode& CBoostedTreeImpl::root(TNodeVec& tree) {
    return tree[0];
}

CBoostedTreeImpl::TVector CBoostedTreeImpl::predictRow(const CEncodedDataFrameRowRef& row,
                                                       const TNodeVecVec& forest) const {
    TVector result{TVector::Zero(m_Loss->numberParameters())};
    for (const auto& tree : forest) {
        result += root(tree).value(row, tree);
    }
    return result;
}

bool CBoostedTreeImpl::selectNextHyperparameters(const TMeanVarAccumulator& lossMoments,
                                                 CBayesianOptimisation& bopt) {

    TVector parameters{this->numberHyperparametersToTune()};

    // Read parameters for last round.
    int i{0};
    if (m_DownsampleFactorOverride == boost::none) {
        parameters(i++) = CTools::stableLog(m_DownsampleFactor);
    }
    if (m_RegularizationOverride.depthPenaltyMultiplier() == boost::none) {
        parameters(i++) = CTools::stableLog(m_Regularization.depthPenaltyMultiplier());
    }
    if (m_RegularizationOverride.leafWeightPenaltyMultiplier() == boost::none) {
        parameters(i++) = CTools::stableLog(m_Regularization.leafWeightPenaltyMultiplier());
    }
    if (m_RegularizationOverride.treeSizePenaltyMultiplier() == boost::none) {
        parameters(i++) = CTools::stableLog(m_Regularization.treeSizePenaltyMultiplier());
    }
    if (m_RegularizationOverride.softTreeDepthLimit() == boost::none) {
        parameters(i++) = m_Regularization.softTreeDepthLimit();
    }
    if (m_RegularizationOverride.softTreeDepthTolerance() == boost::none) {
        parameters(i++) = m_Regularization.softTreeDepthTolerance();
    }
    if (m_EtaOverride == boost::none) {
        parameters(i++) = CTools::stableLog(m_Eta);
        parameters(i++) = m_EtaGrowthRatePerTree;
    }
    if (m_FeatureBagFractionOverride == boost::none) {
        parameters(i++) = m_FeatureBagFraction;
    }

    double meanLoss{CBasicStatistics::mean(lossMoments)};
    double lossVariance{CBasicStatistics::variance(lossMoments)};

    LOG_TRACE(<< "round = " << m_CurrentRound << " loss = " << meanLoss
              << ": regularization = " << m_Regularization.print()
              << ", downsample factor = " << m_DownsampleFactor << ", eta = " << m_Eta
              << ", eta growth rate per tree = " << m_EtaGrowthRatePerTree
              << ", feature bag fraction = " << m_FeatureBagFraction);

    bopt.add(parameters, meanLoss, lossVariance);
    if (3 * m_CurrentRound < m_NumberRounds) {
        std::generate_n(parameters.data(), parameters.size(), [&]() {
            return CSampling::uniformSample(m_Rng, 0.0, 1.0);
        });
        TVector minBoundary;
        TVector maxBoundary;
        std::tie(minBoundary, maxBoundary) = bopt.boundingBox();
        parameters = minBoundary + parameters.cwiseProduct(maxBoundary - minBoundary);
    } else {
        std::tie(parameters, std::ignore) = bopt.maximumExpectedImprovement();
    }

    // Downsampling acts as a regularisation and also increases the variance
    // of each of the base learners so we scale the other regularisation terms
    // and the weight shrinkage to compensate.
    double scale{1.0};

    // Write parameters for next round.
    i = 0;
    if (m_DownsampleFactorOverride == boost::none) {
        m_DownsampleFactor = CTools::stableExp(parameters(i++));
        TVector minBoundary;
        TVector maxBoundary;
        std::tie(minBoundary, maxBoundary) = bopt.boundingBox();
        scale = std::min(scale, 2.0 * m_DownsampleFactor /
                                    (CTools::stableExp(minBoundary(0)) +
                                     CTools::stableExp(maxBoundary(0))));
    }
    if (m_RegularizationOverride.depthPenaltyMultiplier() == boost::none) {
        m_Regularization.depthPenaltyMultiplier(CTools::stableExp(parameters(i++)));
    }
    if (m_RegularizationOverride.leafWeightPenaltyMultiplier() == boost::none) {
        m_Regularization.leafWeightPenaltyMultiplier(
            scale * CTools::stableExp(parameters(i++)));
    }
    if (m_RegularizationOverride.treeSizePenaltyMultiplier() == boost::none) {
        m_Regularization.treeSizePenaltyMultiplier(
            scale * CTools::stableExp(parameters(i++)));
    }
    if (m_RegularizationOverride.softTreeDepthLimit() == boost::none) {
        m_Regularization.softTreeDepthLimit(parameters(i++));
    }
    if (m_RegularizationOverride.softTreeDepthTolerance() == boost::none) {
        m_Regularization.softTreeDepthTolerance(parameters(i++));
    }
    if (m_EtaOverride == boost::none) {
        m_Eta = CTools::stableExp(scale * parameters(i++));
        m_EtaGrowthRatePerTree = parameters(i++);
    }
    if (m_FeatureBagFractionOverride == boost::none) {
        m_FeatureBagFraction = parameters(i++);
    }

    return true;
}

void CBoostedTreeImpl::captureBestHyperparameters(const TMeanVarAccumulator& lossMoments,
                                                  std::size_t maximumNumberTrees) {
    // We capture the parameters with the lowest error at one standard
    // deviation above the mean. If the mean error improvement is marginal
    // we prefer the solution with the least variation across the folds.
    double loss{lossAtNSigma(1.0, lossMoments)};
    if (loss < m_BestForestTestLoss) {
        m_BestForestTestLoss = loss;
        m_BestHyperparameters = CBoostedTreeHyperparameters{
            m_Regularization,       m_DownsampleFactor, m_Eta,
            m_EtaGrowthRatePerTree, maximumNumberTrees, m_FeatureBagFraction};
    }
}

void CBoostedTreeImpl::restoreBestHyperparameters() {
    m_Regularization = m_BestHyperparameters.regularization();
    m_DownsampleFactor = m_BestHyperparameters.downsampleFactor();
    m_Eta = m_BestHyperparameters.eta();
    m_EtaGrowthRatePerTree = m_BestHyperparameters.etaGrowthRatePerTree();
    m_MaximumNumberTrees = m_BestHyperparameters.maximumNumberTrees();
    m_FeatureBagFraction = m_BestHyperparameters.featureBagFraction();
    LOG_TRACE(<< "loss* = " << m_BestForestTestLoss
              << ", regularization* = " << m_Regularization.print()
              << ", downsample factor* = " << m_DownsampleFactor << ", eta* = " << m_Eta
              << ", eta growth rate per tree* = " << m_EtaGrowthRatePerTree
              << ", maximum number trees* = " << m_MaximumNumberTrees
              << ", feature bag fraction* = " << m_FeatureBagFraction);
}

std::size_t CBoostedTreeImpl::numberHyperparametersToTune() const {
    return m_RegularizationOverride.countNotSet() +
           (m_DownsampleFactorOverride != boost::none ? 0 : 1) +
           (m_EtaOverride != boost::none ? 0 : 2) +
           (m_FeatureBagFractionOverride != boost::none ? 0 : 1);
}

std::size_t CBoostedTreeImpl::maximumTreeSize(const core::CPackedBitVector& trainingRowMask) const {
    return this->maximumTreeSize(static_cast<std::size_t>(trainingRowMask.manhattan()));
}

std::size_t CBoostedTreeImpl::maximumTreeSize(std::size_t numberRows) const {
    return static_cast<std::size_t>(
        std::ceil(10.0 * std::sqrt(static_cast<double>(numberRows))));
}

void CBoostedTreeImpl::recordHyperparameters() {
    m_Instrumentation->hyperparameters().s_Eta = m_Eta;
    m_Instrumentation->hyperparameters().s_ClassAssignmentObjective = m_ClassAssignmentObjective;
    m_Instrumentation->hyperparameters().s_DownsampleFactor = m_DownsampleFactor;
    m_Instrumentation->hyperparameters().s_NumFolds = m_NumberFolds;
    m_Instrumentation->hyperparameters().s_MaxTrees = m_MaximumNumberTrees;
    m_Instrumentation->hyperparameters().s_FeatureBagFraction = m_FeatureBagFraction;
    m_Instrumentation->hyperparameters().s_EtaGrowthRatePerTree = m_EtaGrowthRatePerTree;
    m_Instrumentation->hyperparameters().s_MaxAttemptsToAddTree = m_MaximumAttemptsToAddTree;
    m_Instrumentation->hyperparameters().s_NumSplitsPerFeature = m_NumberSplitsPerFeature;
    m_Instrumentation->hyperparameters().s_MaxOptimizationRoundsPerHyperparameter =
        m_MaximumOptimisationRoundsPerHyperparameter;
    m_Instrumentation->hyperparameters().s_Regularization =
        CDataFrameTrainBoostedTreeInstrumentationInterface::SRegularization{
            m_Regularization.depthPenaltyMultiplier(),
            m_Regularization.softTreeDepthLimit(),
            m_Regularization.softTreeDepthTolerance(),
            m_Regularization.treeSizePenaltyMultiplier(),
            m_Regularization.leafWeightPenaltyMultiplier()};
}

void CBoostedTreeImpl::startProgressMonitoringFineTuneHyperparameters() {

    // This costs "number folds" * "maximum number trees per forest" units
    // per round.

    m_Instrumentation->startNewProgressMonitoredTask(CBoostedTreeFactory::FINE_TUNING_PARAMETERS);

    std::size_t totalNumberSteps{m_NumberRounds * m_MaximumNumberTrees * m_NumberFolds};
    LOG_TRACE(<< "main loop total number steps = " << totalNumberSteps);
    m_TrainingProgress = core::CLoopProgress{
        totalNumberSteps, m_Instrumentation->progressCallback(), 1.0, 1024};

    // Make sure progress starts where it left off.
    m_TrainingProgress.increment(m_CurrentRound * m_MaximumNumberTrees * m_NumberFolds);
}

void CBoostedTreeImpl::startProgressMonitoringFinalTrain() {
    // The final model training uses more data so it's monitored separately.

    m_Instrumentation->startNewProgressMonitoredTask(CBoostedTreeFactory::FINAL_TRAINING);
    m_TrainingProgress = core::CLoopProgress{
        m_MaximumNumberTrees, m_Instrumentation->progressCallback(), 1.0, 1024};
}

namespace {
const std::string VERSION_7_8_TAG{"7.8"};
const TStrVec SUPPORTED_VERSIONS{VERSION_7_8_TAG};

const std::string BAYESIAN_OPTIMIZATION_TAG{"bayesian_optimization"};
const std::string BEST_FOREST_TAG{"best_forest"};
const std::string BEST_FOREST_TEST_LOSS_TAG{"best_forest_test_loss"};
const std::string BEST_HYPERPARAMETERS_TAG{"best_hyperparameters"};
const std::string CURRENT_ROUND_TAG{"current_round"};
const std::string DEPENDENT_VARIABLE_TAG{"dependent_variable"};
const std::string DOWNSAMPLE_FACTOR_OVERRIDE_TAG{"downsample_factor_override"};
const std::string DOWNSAMPLE_FACTOR_TAG{"downsample_factor"};
const std::string ENCODER_TAG{"encoder"};
const std::string ETA_GROWTH_RATE_PER_TREE_TAG{"eta_growth_rate_per_tree"};
const std::string ETA_OVERRIDE_TAG{"eta_override"};
const std::string ETA_TAG{"eta"};
const std::string FEATURE_BAG_FRACTION_OVERRIDE_TAG{"feature_bag_fraction_override"};
const std::string FEATURE_BAG_FRACTION_TAG{"feature_bag_fraction"};
const std::string FEATURE_DATA_TYPES_TAG{"feature_data_types"};
const std::string FEATURE_SAMPLE_PROBABILITIES_TAG{"feature_sample_probabilities"};
const std::string FOLD_ROUND_TEST_LOSSES_TAG{"fold_round_test_losses"};
const std::string INITIALIZATION_STAGE_TAG{"initialization_progress"};
const std::string LOSS_TAG{"loss"};
const std::string LOSS_NAME_TAG{"loss_name"};
const std::string MAXIMUM_ATTEMPTS_TO_ADD_TREE_TAG{"maximum_attempts_to_add_tree"};
const std::string MAXIMUM_NUMBER_TREES_OVERRIDE_TAG{"maximum_number_trees_override"};
const std::string MAXIMUM_NUMBER_TREES_TAG{"maximum_number_trees"};
const std::string MAXIMUM_OPTIMISATION_ROUNDS_PER_HYPERPARAMETER_TAG{
    "maximum_optimisation_rounds_per_hyperparameter"};
const std::string MISSING_FEATURE_ROW_MASKS_TAG{"missing_feature_row_masks"};
const std::string NUMBER_FOLDS_TAG{"number_folds"};
const std::string NUMBER_FOLDS_OVERRIDE_TAG{"number_folds_override"};
const std::string NUMBER_ROUNDS_TAG{"number_rounds"};
const std::string NUMBER_SPLITS_PER_FEATURE_TAG{"number_splits_per_feature"};
const std::string NUMBER_THREADS_TAG{"number_threads"};
const std::string RANDOM_NUMBER_GENERATOR_TAG{"random_number_generator"};
const std::string REGULARIZATION_TAG{"regularization"};
const std::string REGULARIZATION_OVERRIDE_TAG{"regularization_override"};
const std::string ROWS_PER_FEATURE_TAG{"rows_per_feature"};
const std::string STOP_CROSS_VALIDATION_EARLY_TAG{"stop_cross_validation_eraly"};
const std::string TESTING_ROW_MASKS_TAG{"testing_row_masks"};
const std::string TRAINING_ROW_MASKS_TAG{"training_row_masks"};
const std::string NUMBER_TOP_SHAP_VALUES_TAG{"top_shap_values"};
}

const std::string& CBoostedTreeImpl::bestHyperparametersName() {
    return BEST_HYPERPARAMETERS_TAG;
}

const std::string& CBoostedTreeImpl::bestRegularizationHyperparametersName() {
    return CBoostedTreeHyperparameters::HYPERPARAM_REGULARIZATION_TAG;
}

CBoostedTreeImpl::TStrVec CBoostedTreeImpl::bestHyperparameterNames() {
    return {CBoostedTreeHyperparameters::HYPERPARAM_DOWNSAMPLE_FACTOR_TAG,
            CBoostedTreeHyperparameters::HYPERPARAM_ETA_TAG,
            CBoostedTreeHyperparameters::HYPERPARAM_ETA_GROWTH_RATE_PER_TREE_TAG,
            CBoostedTreeHyperparameters::HYPERPARAM_FEATURE_BAG_FRACTION_TAG,
            TRegularization::REGULARIZATION_DEPTH_PENALTY_MULTIPLIER_TAG,
            TRegularization::REGULARIZATION_TREE_SIZE_PENALTY_MULTIPLIER_TAG,
            TRegularization::REGULARIZATION_LEAF_WEIGHT_PENALTY_MULTIPLIER_TAG,
            TRegularization::REGULARIZATION_SOFT_TREE_DEPTH_LIMIT_TAG,
            TRegularization::REGULARIZATION_SOFT_TREE_DEPTH_TOLERANCE_TAG};
}

void CBoostedTreeImpl::acceptPersistInserter(core::CStatePersistInserter& inserter) const {
    core::CPersistUtils::persist(VERSION_7_8_TAG, "", inserter);
    core::CPersistUtils::persistIfNotNull(BAYESIAN_OPTIMIZATION_TAG,
                                          m_BayesianOptimization, inserter);
    core::CPersistUtils::persist(BEST_FOREST_TEST_LOSS_TAG, m_BestForestTestLoss, inserter);
    core::CPersistUtils::persist(BEST_FOREST_TAG, m_BestForest, inserter);
    core::CPersistUtils::persist(BEST_HYPERPARAMETERS_TAG, m_BestHyperparameters, inserter);
    core::CPersistUtils::persist(CURRENT_ROUND_TAG, m_CurrentRound, inserter);
    core::CPersistUtils::persist(DEPENDENT_VARIABLE_TAG, m_DependentVariable, inserter);
    core::CPersistUtils::persist(DOWNSAMPLE_FACTOR_OVERRIDE_TAG,
                                 m_DownsampleFactorOverride, inserter);
    core::CPersistUtils::persist(DOWNSAMPLE_FACTOR_TAG, m_DownsampleFactor, inserter);
    core::CPersistUtils::persistIfNotNull(ENCODER_TAG, m_Encoder, inserter);
    core::CPersistUtils::persist(ETA_GROWTH_RATE_PER_TREE_TAG,
                                 m_EtaGrowthRatePerTree, inserter);
    core::CPersistUtils::persist(ETA_TAG, m_Eta, inserter);
    core::CPersistUtils::persist(ETA_OVERRIDE_TAG, m_EtaOverride, inserter);
    core::CPersistUtils::persist(FEATURE_BAG_FRACTION_TAG, m_FeatureBagFraction, inserter);
    core::CPersistUtils::persist(FEATURE_BAG_FRACTION_OVERRIDE_TAG,
                                 m_FeatureBagFractionOverride, inserter);
    core::CPersistUtils::persist(FEATURE_DATA_TYPES_TAG, m_FeatureDataTypes, inserter);
    core::CPersistUtils::persist(FEATURE_SAMPLE_PROBABILITIES_TAG,
                                 m_FeatureSampleProbabilities, inserter);
    core::CPersistUtils::persist(FOLD_ROUND_TEST_LOSSES_TAG, m_FoldRoundTestLosses, inserter);
    core::CPersistUtils::persist(INITIALIZATION_STAGE_TAG,
                                 static_cast<int>(m_InitializationStage), inserter);
    if (m_Loss != nullptr) {
        inserter.insertLevel(LOSS_TAG, [this](core::CStatePersistInserter& inserter_) {
            m_Loss->persistLoss(inserter_);
        });
    }
    core::CPersistUtils::persist(MAXIMUM_ATTEMPTS_TO_ADD_TREE_TAG,
                                 m_MaximumAttemptsToAddTree, inserter);
    core::CPersistUtils::persist(MAXIMUM_OPTIMISATION_ROUNDS_PER_HYPERPARAMETER_TAG,
                                 m_MaximumOptimisationRoundsPerHyperparameter, inserter);
    core::CPersistUtils::persist(MAXIMUM_NUMBER_TREES_TAG, m_MaximumNumberTrees, inserter);
    core::CPersistUtils::persist(MAXIMUM_NUMBER_TREES_OVERRIDE_TAG,
                                 m_MaximumNumberTreesOverride, inserter);
    core::CPersistUtils::persist(MISSING_FEATURE_ROW_MASKS_TAG,
                                 m_MissingFeatureRowMasks, inserter);
    core::CPersistUtils::persist(NUMBER_FOLDS_TAG, m_NumberFolds, inserter);
    core::CPersistUtils::persist(NUMBER_FOLDS_OVERRIDE_TAG, m_NumberFoldsOverride, inserter);
    core::CPersistUtils::persist(NUMBER_ROUNDS_TAG, m_NumberRounds, inserter);
    core::CPersistUtils::persist(NUMBER_SPLITS_PER_FEATURE_TAG,
                                 m_NumberSplitsPerFeature, inserter);
    core::CPersistUtils::persist(NUMBER_THREADS_TAG, m_NumberThreads, inserter);
    core::CPersistUtils::persist(NUMBER_TOP_SHAP_VALUES_TAG, m_NumberTopShapValues, inserter);
    inserter.insertValue(RANDOM_NUMBER_GENERATOR_TAG, m_Rng.toString());
    core::CPersistUtils::persist(REGULARIZATION_OVERRIDE_TAG,
                                 m_RegularizationOverride, inserter);
    core::CPersistUtils::persist(REGULARIZATION_TAG, m_Regularization, inserter);
    core::CPersistUtils::persist(ROWS_PER_FEATURE_TAG, m_RowsPerFeature, inserter);
    core::CPersistUtils::persist(STOP_CROSS_VALIDATION_EARLY_TAG,
                                 m_StopCrossValidationEarly, inserter);
    core::CPersistUtils::persist(TESTING_ROW_MASKS_TAG, m_TestingRowMasks, inserter);
    core::CPersistUtils::persist(TRAINING_ROW_MASKS_TAG, m_TrainingRowMasks, inserter);
}

bool CBoostedTreeImpl::acceptRestoreTraverser(core::CStateRestoreTraverser& traverser) {
    if (traverser.name() != VERSION_7_8_TAG) {
        LOG_ERROR(<< "Input error: unsupported state serialization version. "
                  << "Currently supported versions: "
                  << core::CContainerPrinter::print(SUPPORTED_VERSIONS) << ".");
        return false;
    }

    auto restoreLoss = [this](core::CStateRestoreTraverser& traverser_) {
        m_Loss = CLoss::restoreLoss(traverser_);
        return m_Loss != nullptr;
    };

    int initializationStage{static_cast<int>(E_FullyInitialized)};

    do {
        const std::string& name = traverser.name();
        RESTORE_NO_ERROR(BAYESIAN_OPTIMIZATION_TAG,
                         m_BayesianOptimization =
                             std::make_unique<CBayesianOptimisation>(traverser))
        RESTORE(BEST_FOREST_TAG,
                core::CPersistUtils::restore(BEST_FOREST_TAG, m_BestForest, traverser))
        RESTORE(BEST_FOREST_TEST_LOSS_TAG,
                core::CPersistUtils::restore(BEST_FOREST_TEST_LOSS_TAG,
                                             m_BestForestTestLoss, traverser))
        RESTORE(BEST_HYPERPARAMETERS_TAG,
                core::CPersistUtils::restore(BEST_HYPERPARAMETERS_TAG,
                                             m_BestHyperparameters, traverser))
        RESTORE(CURRENT_ROUND_TAG,
                core::CPersistUtils::restore(CURRENT_ROUND_TAG, m_CurrentRound, traverser))
        RESTORE(DEPENDENT_VARIABLE_TAG,
                core::CPersistUtils::restore(DEPENDENT_VARIABLE_TAG,
                                             m_DependentVariable, traverser))
        RESTORE(DOWNSAMPLE_FACTOR_OVERRIDE_TAG,
                core::CPersistUtils::restore(DOWNSAMPLE_FACTOR_OVERRIDE_TAG,
                                             m_DownsampleFactorOverride, traverser))
        RESTORE(DOWNSAMPLE_FACTOR_TAG,
                core::CPersistUtils::restore(DOWNSAMPLE_FACTOR_TAG, m_DownsampleFactor, traverser))
        RESTORE_NO_ERROR(ENCODER_TAG,
                         m_Encoder = std::make_unique<CDataFrameCategoryEncoder>(traverser))
        RESTORE(ETA_GROWTH_RATE_PER_TREE_TAG,
                core::CPersistUtils::restore(ETA_GROWTH_RATE_PER_TREE_TAG,
                                             m_EtaGrowthRatePerTree, traverser))
        RESTORE(ETA_OVERRIDE_TAG,
                core::CPersistUtils::restore(ETA_OVERRIDE_TAG, m_EtaOverride, traverser))
        RESTORE(ETA_TAG, core::CPersistUtils::restore(ETA_TAG, m_Eta, traverser))
        RESTORE(FEATURE_BAG_FRACTION_OVERRIDE_TAG,
                core::CPersistUtils::restore(FEATURE_BAG_FRACTION_OVERRIDE_TAG,
                                             m_FeatureBagFractionOverride, traverser))
        RESTORE(FEATURE_BAG_FRACTION_TAG,
                core::CPersistUtils::restore(FEATURE_BAG_FRACTION_TAG,
                                             m_FeatureBagFraction, traverser))
        RESTORE(FEATURE_DATA_TYPES_TAG,
                core::CPersistUtils::restore(FEATURE_DATA_TYPES_TAG,
                                             m_FeatureDataTypes, traverser));
        RESTORE(FEATURE_SAMPLE_PROBABILITIES_TAG,
                core::CPersistUtils::restore(FEATURE_SAMPLE_PROBABILITIES_TAG,
                                             m_FeatureSampleProbabilities, traverser))
        RESTORE(FOLD_ROUND_TEST_LOSSES_TAG,
                core::CPersistUtils::restore(FOLD_ROUND_TEST_LOSSES_TAG,
                                             m_FoldRoundTestLosses, traverser))
        RESTORE(INITIALIZATION_STAGE_TAG,
                core::CPersistUtils::restore(INITIALIZATION_STAGE_TAG,
                                             initializationStage, traverser))
        RESTORE(LOSS_TAG, traverser.traverseSubLevel(restoreLoss))
        RESTORE(MAXIMUM_ATTEMPTS_TO_ADD_TREE_TAG,
                core::CPersistUtils::restore(MAXIMUM_ATTEMPTS_TO_ADD_TREE_TAG,
                                             m_MaximumAttemptsToAddTree, traverser))
        RESTORE(MAXIMUM_OPTIMISATION_ROUNDS_PER_HYPERPARAMETER_TAG,
                core::CPersistUtils::restore(
                    MAXIMUM_OPTIMISATION_ROUNDS_PER_HYPERPARAMETER_TAG,
                    m_MaximumOptimisationRoundsPerHyperparameter, traverser))
        RESTORE(MAXIMUM_NUMBER_TREES_OVERRIDE_TAG,
                core::CPersistUtils::restore(MAXIMUM_NUMBER_TREES_OVERRIDE_TAG,
                                             m_MaximumNumberTreesOverride, traverser))
        RESTORE(MAXIMUM_NUMBER_TREES_TAG,
                core::CPersistUtils::restore(MAXIMUM_NUMBER_TREES_TAG,
                                             m_MaximumNumberTrees, traverser))
        RESTORE(MISSING_FEATURE_ROW_MASKS_TAG,
                core::CPersistUtils::restore(MISSING_FEATURE_ROW_MASKS_TAG,
                                             m_MissingFeatureRowMasks, traverser))
        RESTORE(NUMBER_FOLDS_TAG,
                core::CPersistUtils::restore(NUMBER_FOLDS_TAG, m_NumberFolds, traverser))
        RESTORE(NUMBER_FOLDS_OVERRIDE_TAG,
                core::CPersistUtils::restore(NUMBER_FOLDS_OVERRIDE_TAG,
                                             m_NumberFoldsOverride, traverser))
        RESTORE(NUMBER_ROUNDS_TAG,
                core::CPersistUtils::restore(NUMBER_ROUNDS_TAG, m_NumberRounds, traverser))
        RESTORE(NUMBER_SPLITS_PER_FEATURE_TAG,
                core::CPersistUtils::restore(NUMBER_SPLITS_PER_FEATURE_TAG,
                                             m_NumberSplitsPerFeature, traverser))
        RESTORE(NUMBER_THREADS_TAG,
                core::CPersistUtils::restore(NUMBER_THREADS_TAG, m_NumberThreads, traverser))
        RESTORE(NUMBER_TOP_SHAP_VALUES_TAG,
                core::CPersistUtils::restore(NUMBER_TOP_SHAP_VALUES_TAG,
                                             m_NumberTopShapValues, traverser))
        RESTORE(RANDOM_NUMBER_GENERATOR_TAG, m_Rng.fromString(traverser.value()))
        RESTORE(REGULARIZATION_TAG,
                core::CPersistUtils::restore(REGULARIZATION_TAG, m_Regularization, traverser))
        RESTORE(REGULARIZATION_OVERRIDE_TAG,
                core::CPersistUtils::restore(REGULARIZATION_OVERRIDE_TAG,
                                             m_RegularizationOverride, traverser))
        RESTORE(ROWS_PER_FEATURE_TAG,
                core::CPersistUtils::restore(ROWS_PER_FEATURE_TAG, m_RowsPerFeature, traverser))
        RESTORE(STOP_CROSS_VALIDATION_EARLY_TAG,
                core::CPersistUtils::restore(STOP_CROSS_VALIDATION_EARLY_TAG,
                                             m_StopCrossValidationEarly, traverser))
        RESTORE(TESTING_ROW_MASKS_TAG,
                core::CPersistUtils::restore(TESTING_ROW_MASKS_TAG, m_TestingRowMasks, traverser))
        RESTORE(TRAINING_ROW_MASKS_TAG,
                core::CPersistUtils::restore(TRAINING_ROW_MASKS_TAG, m_TrainingRowMasks, traverser))
    } while (traverser.next());

    m_InitializationStage = static_cast<EInitializationStage>(initializationStage);

    return true;
}

std::size_t CBoostedTreeImpl::memoryUsage() const {
    std::size_t mem{core::CMemory::dynamicSize(m_Loss)};
    mem += core::CMemory::dynamicSize(m_Encoder);
    mem += core::CMemory::dynamicSize(m_FeatureDataTypes);
    mem += core::CMemory::dynamicSize(m_FeatureSampleProbabilities);
    mem += core::CMemory::dynamicSize(m_MissingFeatureRowMasks);
    mem += core::CMemory::dynamicSize(m_TrainingRowMasks);
    mem += core::CMemory::dynamicSize(m_TestingRowMasks);
    mem += core::CMemory::dynamicSize(m_FoldRoundTestLosses);
    mem += core::CMemory::dynamicSize(m_BestForest);
    mem += core::CMemory::dynamicSize(m_BayesianOptimization);
    mem += core::CMemory::dynamicSize(m_Instrumentation);
    return mem;
}

void CBoostedTreeImpl::accept(CBoostedTree::CVisitor& visitor) {
    m_Encoder->accept(visitor);
    for (const auto& tree : m_BestForest) {
        visitor.addTree();
        for (const auto& node : tree) {
            node.accept(visitor);
        }
    }
    visitor.addClassificationWeights(m_ClassificationWeights.to<TDoubleVec>());
}

const CBoostedTreeHyperparameters& CBoostedTreeImpl::bestHyperparameters() const {
    return m_BestHyperparameters;
}

CTreeShapFeatureImportance* CBoostedTreeImpl::shap() {
    return m_TreeShap.get();
}

const CBoostedTreeImpl::TDoubleVec& CBoostedTreeImpl::featureSampleProbabilities() const {
    return m_FeatureSampleProbabilities;
}

const CBoostedTreeImpl::TNodeVecVec& CBoostedTreeImpl::trainedModel() const {
    return m_BestForest;
}

CBoostedTreeImpl::TLossFunction& CBoostedTreeImpl::loss() const {
    if (m_Loss == nullptr) {
        HANDLE_FATAL(<< "Internal error: loss function unavailable. "
                     << "Please report this problem.")
    }
    return *m_Loss;
}

std::size_t CBoostedTreeImpl::columnHoldingDependentVariable() const {
    return m_DependentVariable;
}

const CBoostedTreeImpl::TSizeVec& CBoostedTreeImpl::extraColumns() const {
    return m_ExtraColumns;
}

CBoostedTreeImpl::TVector CBoostedTreeImpl::classificationWeights() const {
    return m_ClassificationWeights;
}

const double CBoostedTreeImpl::MINIMUM_RELATIVE_GAIN_PER_SPLIT{1e-7};
}
}
