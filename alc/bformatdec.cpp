
#include "config.h"

#include "bformatdec.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <iterator>
#include <numeric>

#include "AL/al.h"

#include "almalloc.h"
#include "alu.h"
#include "ambdec.h"
#include "filters/splitter.h"
#include "opthelpers.h"


namespace {

constexpr std::array<float,MAX_AMBI_ORDER+1> Ambi3DDecoderHFScale{{
    1.00000000e+00f, 1.00000000e+00f
}};
constexpr std::array<float,MAX_AMBI_ORDER+1> Ambi3DDecoderHFScale2O{{
    7.45355990e-01f, 1.00000000e+00f, 1.00000000e+00f
}};
constexpr std::array<float,MAX_AMBI_ORDER+1> Ambi3DDecoderHFScale3O{{
    5.89792205e-01f, 8.79693856e-01f, 1.00000000e+00f, 1.00000000e+00f
}};

inline auto GetDecoderHFScales(ALuint order) noexcept -> const std::array<float,MAX_AMBI_ORDER+1>&
{
    if(order >= 3) return Ambi3DDecoderHFScale3O;
    if(order == 2) return Ambi3DDecoderHFScale2O;
    return Ambi3DDecoderHFScale;
}

inline auto GetAmbiScales(AmbDecScale scaletype) noexcept
    -> const std::array<float,MAX_AMBI_CHANNELS>&
{
    if(scaletype == AmbDecScale::FuMa) return AmbiScale::FromFuMa;
    if(scaletype == AmbDecScale::SN3D) return AmbiScale::FromSN3D;
    return AmbiScale::FromN3D;
}

} // namespace


BFormatDec::BFormatDec(const AmbDecConf *conf, const bool allow_2band, const ALuint inchans,
    const ALuint srate, const ALuint (&chanmap)[MAX_OUTPUT_CHANNELS])
{
    mDualBand = allow_2band && (conf->FreqBands == 2);
    if(!mDualBand)
        mSamples.resize(2);
    else
    {
        ASSUME(inchans > 0);
        mSamples.resize(inchans * 2);
        mSamplesHF = mSamples.data();
        mSamplesLF = mSamplesHF + inchans;
    }
    mNumChannels = inchans;

    mEnabled = std::accumulate(std::begin(chanmap), std::begin(chanmap)+conf->Speakers.size(), 0u,
        [](ALuint mask, const ALuint &chan) noexcept -> ALuint
        { return mask | (1 << chan); });

    const bool periphonic{(conf->ChanMask&AMBI_PERIPHONIC_MASK) != 0};
    const std::array<float,MAX_AMBI_CHANNELS> &coeff_scale = GetAmbiScales(conf->CoeffScale);
    const size_t coeff_count{periphonic ? MAX_AMBI_CHANNELS : MAX_AMBI2D_CHANNELS};

    if(!mDualBand)
    {
        for(size_t i{0u};i < conf->Speakers.size();i++)
        {
            ALfloat (&mtx)[MAX_AMBI_CHANNELS] = mMatrix.Single[chanmap[i]];
            for(size_t j{0},k{0};j < coeff_count;j++)
            {
                const size_t acn{periphonic ? j : AmbiIndex::From2D[j]};
                if(!(conf->ChanMask&(1u<<acn))) continue;
                mtx[j] = conf->HFMatrix[i][k] / coeff_scale[acn] *
                    conf->HFOrderGain[AmbiIndex::OrderFromChannel[acn]];
                ++k;
            }
        }
    }
    else
    {
        mXOver[0].init(conf->XOverFreq / static_cast<float>(srate));
        std::fill(std::begin(mXOver)+1, std::end(mXOver), mXOver[0]);

        const float ratio{std::pow(10.0f, conf->XOverRatio / 40.0f)};
        for(size_t i{0u};i < conf->Speakers.size();i++)
        {
            ALfloat (&mtx)[sNumBands][MAX_AMBI_CHANNELS] = mMatrix.Dual[chanmap[i]];
            for(size_t j{0},k{0};j < coeff_count;j++)
            {
                const size_t acn{periphonic ? j : AmbiIndex::From2D[j]};
                if(!(conf->ChanMask&(1u<<acn))) continue;
                mtx[sHFBand][j] = conf->HFMatrix[i][k] / coeff_scale[acn] *
                    conf->HFOrderGain[AmbiIndex::OrderFromChannel[acn]] * ratio;
                mtx[sLFBand][j] = conf->LFMatrix[i][k] / coeff_scale[acn] *
                    conf->LFOrderGain[AmbiIndex::OrderFromChannel[acn]] / ratio;
                ++k;
            }
        }
    }
}

BFormatDec::BFormatDec(const ALuint inchans, const ChannelDec (&chancoeffs)[MAX_OUTPUT_CHANNELS],
    const al::span<const ALuint> chanmap)
{
    mSamples.resize(2);
    mNumChannels = inchans;

    mEnabled = std::accumulate(chanmap.begin(), chanmap.end(), 0u,
        [](ALuint mask, const ALuint &chan) noexcept -> ALuint
        { return mask | (1 << chan); });

    const ChannelDec *incoeffs{chancoeffs};
    auto set_coeffs = [this,inchans,&incoeffs](const ALuint chanidx) noexcept -> void
    {
        ALfloat (&mtx)[MAX_AMBI_CHANNELS] = mMatrix.Single[chanidx];
        const ALfloat (&coeffs)[MAX_AMBI_CHANNELS] = *(incoeffs++);

        ASSUME(inchans > 0);
        std::copy_n(std::begin(coeffs), inchans, std::begin(mtx));
    };
    std::for_each(chanmap.begin(), chanmap.end(), set_coeffs);
}


void BFormatDec::process(const al::span<FloatBufferLine> OutBuffer,
    const FloatBufferLine *InSamples, const size_t SamplesToDo)
{
    ASSUME(SamplesToDo > 0);

    if(mDualBand)
    {
        for(ALuint i{0};i < mNumChannels;i++)
            mXOver[i].process({InSamples[i].data(), SamplesToDo}, mSamplesHF[i].data(),
                mSamplesLF[i].data());

        ALfloat (*mixmtx)[sNumBands][MAX_AMBI_CHANNELS]{mMatrix.Dual};
        ALuint enabled{mEnabled};
        for(FloatBufferLine &outbuf : OutBuffer)
        {
            if LIKELY(enabled&1)
            {
                const al::span<float> outspan{outbuf.data(), SamplesToDo};
                MixRowSamples(outspan, {(*mixmtx)[sHFBand], mNumChannels}, mSamplesHF->data(),
                    mSamplesHF->size());
                MixRowSamples(outspan, {(*mixmtx)[sLFBand], mNumChannels}, mSamplesLF->data(),
                    mSamplesLF->size());
            }
            ++mixmtx;
            enabled >>= 1;
        }
    }
    else
    {
        ALfloat (*mixmtx)[MAX_AMBI_CHANNELS]{mMatrix.Single};
        ALuint enabled{mEnabled};
        for(FloatBufferLine &outbuf : OutBuffer)
        {
            if LIKELY(enabled&1)
                MixRowSamples({outbuf.data(), SamplesToDo}, {*mixmtx, mNumChannels},
                    InSamples->data(), InSamples->size());
            ++mixmtx;
            enabled >>= 1;
        }
    }
}


auto BFormatDec::GetHFOrderScales(const ALuint in_order, const ALuint out_order) noexcept
    -> std::array<float,MAX_AMBI_ORDER+1>
{
    std::array<float,MAX_AMBI_ORDER+1> ret{};

    assert(out_order >= in_order);

    const auto &target = GetDecoderHFScales(out_order);
    const auto &input = GetDecoderHFScales(in_order);

    for(size_t i{0};i < in_order+1;++i)
        ret[i] = input[i] / target[i];

    return ret;
}
