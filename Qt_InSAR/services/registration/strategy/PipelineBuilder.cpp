#include "PipelineBuilder.h"

#include "services/registration/steps/DataReader.h"
#include "services/registration/steps/TOPSARDeramp.h"
#include "services/registration/steps/BurstMatcher.h"
#include "services/registration/steps/OrbitInitializer.h"
#include "services/registration/steps/CoarseCorrelator.h"
#include "services/registration/steps/OffsetExtractor.h"
#include "services/registration/steps/FineCorrelator.h"
#include "services/registration/steps/PolynomialFitter.h"
#include "services/registration/steps/EsdCorrector.h"
#include "services/registration/steps/SincResampler.h"
#include "services/registration/steps/QualityEvaluator.h"

QVector<IRegStep*> buildPipelineSteps(const RegistrationStrategy& strat)
{
    Q_UNUSED(strat)

    // 固定顺序约束: FineCorrelator 必须在 PolynomialFitter 之前
    // (多项式需要在精化后的点上拟合)
    QVector<IRegStep*> steps;
    steps << new DataReader
          << new TOPSARDeramp
          << new BurstMatcher
          << new OrbitInitializer
          << new CoarseCorrelator
          << new OffsetExtractor
          << new FineCorrelator
          << new PolynomialFitter
          << new EsdCorrector
          << new SincResampler
          << new QualityEvaluator;
    return steps;
}
