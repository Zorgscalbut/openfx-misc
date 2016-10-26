/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of openfx-misc <https://github.com/devernay/openfx-misc>,
 * Copyright (C) 2013-2016 INRIA
 *
 * openfx-misc is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * openfx-misc is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with openfx-misc.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

/*
 * OFX SlitScan plugin.
 */

/*
   [JCL]
   • J'aimerai bien que l'on aie dans les nodes de retiming un Slitscan (qui manque à Nuke) :
   entrée 1 : source
   entrée 2 : retime Map

   Pour chaque pixel l'offset de timing est déterminé par la valeur de la retime Map
   On peut avoir un mode de retime map absolu en virgule flottante et un autre mappé de 0 à 1 avec un Range à définir (pas de retime pour 0.5).

   Fred tu nous fais ça ? :)

   [CoyHot]
   Une question à se posait aussi : à quoi ressemblerait la retime map :

   -  Min/max entre -1 et 1 avec un facteur multiplicateur ?
   - Greyscale visible pour que ça soit plus compréhensible avec mid-grey à 0.5 qui ne bouge pas et les valeur plus sombre en négatif et plus clair en positif ?

   [JCL]
   C'était un peu ce que je proposait :
   On peut avoir un mode de retime map absolu en virgule flottante (la valeur vaut le décalage d'image) et un autre mappé de 0 à 1 avec un Range à définir (pas de retime pour 0.5).

   [Fred]
   pour les valeurs qui tombent entre deux images, vous espérez quoi?

   - image la plus proche?
   - blend linéaire entre les deux images les plus proches? (risque d'être moche)
   - retiming? (ce sera pas pour tout de suite... il y a masse de boulot pour faire un bon retiming)

   Il y aura un paramètre un paramètre d'offset (valeur par défaut -0.5), et un paramètre de gain, évidemment, parce que ce qui est rigolo c'est de jouer avec ce paramètre.

   valeurs par défaut: offset à -0.5, multiplicateur à 10, frame range -5..5

   pour passer en mode "retime map en nombre de frames", il suffirait de mettre l'offset à 0 et le multiplicateur à 1. Ou bien est-ce qu'il faut une case à cocher pour forcer ce mode (qui désactiverait les deux autres paramètres?)

   il faudrait aussi une case à cocher "absolute". Dans ce cas le frame range et les valeurs données dans la retime map sont absolues.

   Et il y aura aussi un paramètre pourri qui s'appellera "maximum input frame range". Ce paramètre permettra de préfetcher toutes les images nécessaires à l'effet avant son exécution. On pourra dépasser ce range sous Natron, mais pas sous Nuke (Natron est plus tolérant).
 */

#include <cmath> // for floor
#include <climits> // for INT_MAX
#include <cassert>
#include <set>
#include <map>

#include "ofxsImageEffect.h"
#include "ofxsMultiThread.h"

#include "ofxsPixelProcessor.h"
#include "ofxsMaskMix.h"
#include "ofxsCoords.h"
#include "ofxsCopier.h"
#include "ofxsMacros.h"

using namespace OFX;

OFXS_NAMESPACE_ANONYMOUS_ENTER

#define kPluginName "SlitScan"
#define kPluginGrouping "Time"
#define kPluginDescription \
    "Apply per-pixel retiming: the time is computed for each pixel from the retime function, which can be either a horizontal ramp, a vertical ramp, or a retime map.\n" \
    "\n" \
    "The default retime function corresponds to a horizontal slit: it is a vertical ramp, which is a linear function of y, which is 0 at the center of the bottom image line, and 1 at the center of the top image line. Optionally, a vertical slit may be used (0 at the center of the leftmost image column, 1 at the center of the rightmost image column), or the optional single-channel \"Retime Map\" input may also be used.\n" \
    "\n" \
    "This plugin requires to render many frames on input, which may require a lot of memory.\n" \
    "Note that the results may be on higher quality if the video is slowed fown (e.g. using slowmoVideo)\n" \
    "\n" \
    "The parameters are:\n" \
    "- retime function (default = horizontal slit)\n" \
    "- offset for the retime function (default = 0)\n" \
    "- gain for the retime function (default = -10)\n" \
    "- absolute, a boolean indicating that the time map gives absolute frames rather than relative frames\n" \
    "- frame range, only if RetimeMap is connected, because the frame range depends on the retime map content (default = -10..0). If \"absolute\" is checked, this frame range is absolute, else it is relative to the current frame\n" \
    "- filter to handle time offsets that \"fall between\" frames. They can be mapped to the nearest frame, or interpolated between the nearest frames (corresponding to a shutter of 1 frame).\n" \


#define kPluginIdentifier "net.sf.openfx.SlitScan"
// History:
// version 1.0: initial version
#define kPluginVersionMajor 1 // Incrementing this number means that you have broken backwards compatibility of the plug-in.
#define kPluginVersionMinor 0 // Increment this when you have fixed a bug or made it faster.

#define kSupportsTiles 1
#define kSupportsMultiResolution 1
#define kSupportsRenderScale 1
#define kSupportsMultipleClipPARs false
#define kSupportsMultipleClipDepths false
#define kRenderThreadSafety eRenderFullySafe

#define kClipRetimeMap "Retime Map"

#define kParamRetimeFunction "retimeFunction"
#define kParamRetimeFunctionLabel "Retime Function"
#define kParamRetimeFunctionHint "The function that gives, for each pixel in the image, its time. The default retime function corresponds to a horizontal slit: it is a vertical ramp (a linear function of y) which is 0 at the center of the bottom image line, and 1 at the center of the top image line. Optionally, a vertical slit may be used (0 at the center of the leftmost image column, 1 at the center of the rightmost image column), or the optional single-channel \"Retime Map\" input may also be used."
#define kParamRetimeFunctionOptionHorizontalSlit "Horizontal Slit"
#define kParamRetimeFunctionOptionHorizontalSlitHint "A vertical ramp (a linear function of y) which is 0 at the center of the bottom image line, and 1 at the center of the top image line."
#define kParamRetimeFunctionOptionVerticalSlit "Vertical Slit"
#define kParamRetimeFunctionOptionVerticalSlitHint "A horizontal ramp (alinear function of x) which is 0 at the center of the leftmost image line, and 1 at the center of the rightmost image line."
#define kParamRetimeFunctionOptionRetimeMap "Retime Map"
#define kParamRetimeFunctionOptionRetimeMapHint "The single-channel image from the \"Retime Map\" input (zero if not connected)."
#define kParamRetimeFunctionDefault eRetimeFunctionHorizontalSlit
enum RetimeFunctionEnum
{
    eRetimeFunctionHorizontalSlit,
    eRetimeFunctionVerticalSlit,
    eRetimeFunctionRetimeMap,
};

#define kParamRetimeFunctionOptionVertical
#define kParamRetimeOffset "retimeOffset"
#define kParamRetimeOffsetLabel "Retime Offset"
#define kParamRetimeOffsetHint "Offset to the retime map."
#define kParamRetimeOffsetDefault 0.

#define kParamRetimeGain "retimeGain"
#define kParamRetimeGainLabel "Retime Gain"
#define kParamRetimeGainHint "Gain applied to the retime map (after offset). With the horizontal or vertical slits, to get one line or column per frame you should use respectively (height-1) or (width-1)."
#define kParamRetimeGainDefault -10

#define kParamRetimeAbsolute "retimeAbsolute"
#define kParamRetimeAbsoluteLabel "Absolute"
#define kParamRetimeAbsoluteHint "If checked, the retime map contains absolute time, if not it is relative to the current frame."
#define kParamRetimeAbsoluteDefault false

#define kParamFrameRange "frameRange"
#define kParamFrameRangeLabel "Max. Frame Range"
#define kParamFrameRangeHint "Maximum input frame range to fetch images from (may be relative or absolute, depending on the \"absolute\" parameter). Only used if the Retime Map is used and connected."
#define kParamFrameRangeDefault -10, 0

#define kParamFilter "filter"
#define kParamFilterLabel "Filter"
#define kParamFilterHint "How input images are combined to compute the output image."

#define kParamFilterOptionNearest "Nearest"
#define kParamFilterOptionNearestHint "Pick input image with nearest integer time."
#define kParamFilterOptionLinear "Linear"
#define kParamFilterOptionLinearHint "Blend the two nearest images with linear interpolation."
// TODO:
#define kParamFilterOptionBox "Box"
#define kParamFilterOptionBoxHint "Weighted average of images over the shutter time (shutter time is defined in the output sequence)." // requires shutter parameter

enum FilterEnum
{
    //eFilterNone, // nonsensical with SlitScan
    eFilterNearest,
    eFilterLinear,
    //eFilterBox,
};

#define kParamFilterDefault eFilterNearest

class SourceImages
{
public:
    SourceImages(const OFX::ImageEffect& effect, OFX::Clip *srcClip)
        : _effect(effect)
        , _srcClip(srcClip)
    {
    }

    ~SourceImages()
    {
        for (ImagesMap::const_iterator it = _images.begin();
             it != _images.end();
             ++it) {
            delete it->second;
        }
    }

    const OFX::Image* fetch(double time, bool nofetch = false) const
    {
        ImagesMap::const_iterator it = _images.find(time);
        if (it != _images.end()) {
            return it->second;
        }
        if (nofetch) {
            assert(false);
            return NULL;
        }
        return _images[time] = _srcClip->fetchImage(time);
    }

    void fetchSet(const std::set<double> &times) const
    {
        for (std::set<double>::const_iterator it = times.begin();
             it != times.end() && !_effect.abort();
             ++it) {
            //printf("fetching %g\n", *it);
            fetch(*it);
        }
    }

    /** @brief return a pixel pointer, returns NULL if (x,y) is outside the image bounds

     x and y are in pixel coordinates

     If the components are custom, then this will return NULL as the support code
     can't know the pixel size to do the work.
     */
    const void *getPixelAddress(double time, int x, int y) const
    {
        const OFX::Image* img = fetch(time, true);
        if (img) {
            return img->getPixelAddress(x, y);
        }
        return NULL;
    }

private:
    typedef std::map<double, const OFX::Image*> ImagesMap;

    const OFX::ImageEffect& _effect;
    OFX::Clip *_srcClip;            /**< @brief Mandated input clips */

    mutable ImagesMap _images;
};

class SlitScanProcessorBase;

////////////////////////////////////////////////////////////////////////////////
/** @brief The plugin that does our work */
class SlitScanPlugin
    : public OFX::ImageEffect
{
protected:
    // do not need to delete these, the ImageEffect is managing them for us
    OFX::Clip *_dstClip;            /**< @brief Mandated output clips */
    OFX::Clip *_srcClip;            /**< @brief Mandated input clips */
    OFX::Clip *_retimeMapClip;      /**< @brief Optional retime map */
    OFX::ChoiceParam  *_retimeFunction;
    OFX::DoubleParam  *_retimeOffset;
    OFX::DoubleParam  *_retimeGain;
    OFX::BooleanParam *_retimeAbsolute;
    OFX::Int2DParam *_frameRange;
    OFX::ChoiceParam *_filter;   /**< @brief how images are interpolated (or not). */

public:
    /** @brief ctor */
    SlitScanPlugin(OfxImageEffectHandle handle)
        : ImageEffect(handle)
        , _dstClip(0)
        , _srcClip(0)
        , _retimeMapClip(0)
        , _retimeFunction(0)
        , _retimeOffset(0)
        , _retimeGain(0)
        , _retimeAbsolute(0)
        , _frameRange(0)
        , _filter(0)
    {
        _dstClip = fetchClip(kOfxImageEffectOutputClipName);
        _srcClip = fetchClip(kOfxImageEffectSimpleSourceClipName);
        _retimeMapClip = fetchClip(kClipRetimeMap);

        _retimeFunction = fetchChoiceParam(kParamRetimeFunction);
        _retimeOffset = fetchDoubleParam(kParamRetimeOffset);
        _retimeGain = fetchDoubleParam(kParamRetimeGain);
        _retimeAbsolute = fetchBooleanParam(kParamRetimeAbsolute);
        _frameRange = fetchInt2DParam(kParamFrameRange);
        _filter = fetchChoiceParam(kParamFilter);
        assert(_retimeFunction && _retimeOffset && _retimeGain && _retimeAbsolute && _frameRange && _filter);
        updateVisibility();
    }

private:
    /* Override the render */
    virtual void render(const OFX::RenderArguments &args) OVERRIDE FINAL;

    template <int nComponents>
    void renderForComponents(const OFX::RenderArguments &args);

    /** Override the get frames needed action */
    virtual void getFramesNeeded(const OFX::FramesNeededArguments &args, OFX::FramesNeededSetter &frames) OVERRIDE FINAL;
    virtual bool isIdentity(const OFX::IsIdentityArguments &args, OFX::Clip * &identityClip, double &identityTime) OVERRIDE FINAL;
    virtual bool getRegionOfDefinition(const OFX::RegionOfDefinitionArguments &args, OfxRectD &rod) OVERRIDE FINAL;
    virtual void changedParam(const OFX::InstanceChangedArgs &args, const std::string &paramName) OVERRIDE FINAL
    {
        if ( (paramName == kParamRetimeFunction) && (args.reason == OFX::eChangeUserEdit) ) {
            updateVisibility();
        }
    }

    /* set up and run a processor */
    void setupAndProcess(SlitScanProcessorBase &, const OFX::RenderArguments &args);
    void getFramesNeededRange(const double time,
                              OfxRangeD &range);
    void updateVisibility()
    {
        RetimeFunctionEnum retimeFunction = (RetimeFunctionEnum)_retimeFunction->getValue();
        _frameRange->setEnabled(retimeFunction == eRetimeFunctionRetimeMap);
    }

};


////////////////////////////////////////////////////////////////////////////////
/** @brief render for the filter */

////////////////////////////////////////////////////////////////////////////////
// basic plugin render function, just a skelington to instantiate templates from

void
SlitScanPlugin::getFramesNeeded(const OFX::FramesNeededArguments &args,
                                OFX::FramesNeededSetter &frames)
{
    if (!_srcClip) {
        return;
    }
    const double time = args.time;
    OfxRangeD range = {time, time};
    getFramesNeededRange(time, range);
    frames.setFramesNeeded(*_srcClip, range);
}

void
SlitScanPlugin::getFramesNeededRange(const double time,
                                     OfxRangeD &range)
{
    double tmin, tmax;
    bool retimeAbsolute = _retimeAbsolute->getValueAtTime(time);
    RetimeFunctionEnum retimeFunction = (RetimeFunctionEnum)_retimeFunction->getValueAtTime(time);
    if (retimeFunction == eRetimeFunctionRetimeMap) {
        int t1, t2;
        _frameRange->getValueAtTime(time, t1, t2);
        if (retimeAbsolute) {
            tmin = std::min(t1, t2);
            tmax = std::max(t1, t2);
        } else {
            tmin = time + std::min(t1, t2);
            tmax = time + std::max(t1, t2);
        }
    } else {
        double retimeOffset = _retimeOffset->getValueAtTime(time);
        double retimeGain = _retimeGain->getValueAtTime(time);
        tmin = (retimeGain >  0) ? retimeOffset : retimeOffset + retimeGain;
        tmax = (retimeGain <= 0) ? retimeOffset : retimeOffset + retimeGain;
        if (!retimeAbsolute) {
            tmin += time;
            tmax += time;
        }
        FilterEnum filter = (FilterEnum)_filter->getValueAtTime(time);
        if (filter == eFilterNearest) {
            tmin = std::floor(tmin + 0.5);
            tmax = std::floor(tmax + 0.5);
        } else if (filter == eFilterLinear) {
            tmin = std::floor(tmin);
            tmax = std::ceil(tmax);
        }
    }

    range.min = tmin;
    range.max = tmax;
}

bool
SlitScanPlugin::isIdentity(const OFX::IsIdentityArguments &args,
                           OFX::Clip * &identityClip,
                           double &identityTime)
{
    const double time = args.time;
    double retimeGain = _retimeGain->getValueAtTime(time);

    RetimeFunctionEnum retimeFunction = (RetimeFunctionEnum)_retimeFunction->getValueAtTime(time);
    if ( (retimeFunction == eRetimeFunctionRetimeMap) && !( _retimeMapClip && _retimeMapClip->isConnected() ) ) {
        // no retime map, equivalent to value = 0 everywhere
        retimeGain = 0.;
    }

    if (retimeGain == 0.) {
        double retimeOffset = _retimeOffset->getValueAtTime(time);
        bool retimeAbsolute = _retimeAbsolute->getValueAtTime(time);
        identityTime = retimeAbsolute ? retimeOffset : (time + retimeOffset);
        if (identityTime != (int)identityTime) {
            FilterEnum filter = (FilterEnum)_filter->getValueAtTime(time);
            if (filter == eFilterNearest) {
                identityTime = std::floor(identityTime + 0.5);
            } else {
                return false; // result is blended
            }
        }
        identityClip = _srcClip;

        return true;
    }

    return false;
}

bool
SlitScanPlugin::getRegionOfDefinition(const OFX::RegionOfDefinitionArguments &args,
                                      OfxRectD &rod)
{
    const double time = args.time;
    double retimeGain;

    _retimeGain->getValueAtTime(time, retimeGain);

    RetimeFunctionEnum retimeFunction = (RetimeFunctionEnum)_retimeFunction->getValueAtTime(time);
    if ( (retimeFunction == eRetimeFunctionRetimeMap) && !( _retimeMapClip && _retimeMapClip->isConnected() ) ) {
        // no retime map, equivalent to value = 0 everywhere
        retimeGain = 0.;
    }

    if (retimeGain == 0.) {
        double retimeOffset;
        _retimeOffset->getValueAtTime(time, retimeOffset);
        bool retimeAbsolute;
        _retimeAbsolute->getValueAtTime(time, retimeAbsolute);
        double identityTime = retimeAbsolute ? retimeOffset : (time + retimeOffset);
        if (identityTime != (int)identityTime) {
            FilterEnum filter = (FilterEnum)_filter->getValueAtTime(time);
            if (filter == eFilterNearest) {
                identityTime = std::floor(identityTime + 0.5);
            } else {
                return false; // result is blended
            }
        }
        rod = _srcClip->getRegionOfDefinition(identityTime);

        return true;
    }

    return false;
}

// build the set of times needed to render renderWindow
template<class PIX, int maxValue>
void
buildTimes(const OFX::ImageEffect& effect,
           const OFX::Image* retimeMap,
           double time,
           const OfxRectI& renderWindow,
           double retimeGain,
           double retimeOffset,
           bool retimeAbsolute,
           FilterEnum filter,
           std::set<double> *sourceImagesTimes)
{
    for (int y = renderWindow.y1; y < renderWindow.y2; ++y) {
        if ( effect.abort() ) {
            return;
        }
        for (int x = renderWindow.x1; x < renderWindow.x2; ++x) {
            PIX* mapPix = retimeGain != 0. ? (PIX*)retimeMap->getPixelAddress(x, y) : NULL;
            double mapVal = mapPix ? (double)(*mapPix) / maxValue  : 0.;
            double srcTime = retimeGain * mapVal + retimeOffset;
            if (!retimeAbsolute) {
                srcTime += time;
            }
            if (srcTime == (int)srcTime) {
                sourceImagesTimes->insert(srcTime);
            } else {
                if (filter == eFilterNearest) {
                    sourceImagesTimes->insert( std::floor(srcTime + 0.5) );
                } else {
                    sourceImagesTimes->insert( std::floor(srcTime) );
                    sourceImagesTimes->insert( std::ceil(srcTime) );
                }
            }
        }
    }
}

void
buildTimesSlit(double time,
               double a,
               double b,
               double retimeGain,
               double retimeOffset,
               bool retimeAbsolute,
               FilterEnum filter,
           std::set<double> *sourceImagesTimes)
{
    a = a * retimeGain + retimeOffset;
    b = b * retimeGain + retimeOffset;
    if (!retimeAbsolute) {
        a += time;
        b += time;
    }
    if (a > b) {
        std::swap(a, b);
    }
    int tmin, tmax;
    switch (filter) {
        case eFilterNearest: {
            tmin = std::floor(a + 0.5);
            tmax = std::floor(b + 0.5);
            break;
        }
        case eFilterLinear: {
            tmin = std::floor(a);
            tmax = std::ceil(b);
            break;
        }
    }
    for (int t = tmin; t <= tmax; ++t) {
        sourceImagesTimes->insert(t);
    }
}

class SlitScanProcessorBase
: public OFX::PixelProcessor
{
protected :
    const SourceImages *_sourceImages;
    const OFX::Image *_retimeMap;
    double _time;
    FilterEnum _filter;
    RetimeFunctionEnum _retimeFunction;
    double _retimeGain;
    double _retimeOffset;
    bool _retimeAbsolute;
    OfxRectI _srcRoDPixel;

    public :
    /** @brief no arg ctor */
    SlitScanProcessorBase(OFX::ImageEffect &instance)
    : OFX::PixelProcessor(instance)
    , _sourceImages(0)
    , _retimeMap(0)
    , _time(0)
    , _filter(eFilterNearest)
    , _retimeFunction(eRetimeFunctionHorizontalSlit)
    , _retimeGain(1.)
    , _retimeOffset(0.)
    , _retimeAbsolute(false)
    {
        _srcRoDPixel.x1 = _srcRoDPixel.y1 = _srcRoDPixel.x2 = _srcRoDPixel.y2 = 0;
    }

    /** @brief set the src images */
    void setSourceImages(const SourceImages *v) {_sourceImages = v;}
    void setRetimeMap(const OFX::Image *v)   {_retimeMap = v;}

    void setValues(double time,
                   FilterEnum filter,
                   RetimeFunctionEnum retimeFunction,
                   double retimeGain,
                   double retimeOffset,
                   bool retimeAbsolute,
                   const OfxRectI& srcRoDPixel)
    {
        _time = time;
        _filter = filter;
        _retimeFunction = retimeFunction;
        _retimeGain = retimeGain;
        _retimeOffset = retimeOffset;
        _retimeAbsolute = retimeAbsolute;
        _srcRoDPixel = srcRoDPixel;
    }
};

/** @brief templated class to blend between two images */
template <class PIX, int nComponents, int maxValue>
class SlitScanProcessor
: public SlitScanProcessorBase
{
    public :
    // ctor
    SlitScanProcessor(OFX::ImageEffect &instance)
    : SlitScanProcessorBase(instance)
    {}

    static PIX Lerp(const PIX &v1,
                    const PIX &v2,
                    float blend)
    {
        return PIX((v2 - v1) * blend + v1);
    }

    // and do some processing
    void multiThreadProcessImages(OfxRectI procWindow)
    {
        for(int y = procWindow.y1; y < procWindow.y2; y++) {
            if(_effect.abort()) break;

            PIX *dstPix = (PIX *)getDstPixelAddress(procWindow.x1, y);

            for(int x = procWindow.x1; x < procWindow.x2; x++) {
                double retimeVal;
                switch (_retimeFunction) {
                    case eRetimeFunctionHorizontalSlit: {
                        // A vertical ramp (a linear function of y) which is 0 at the center of the bottom image line, and 1 at the center of the top image line.
                        retimeVal = (double)(y - _srcRoDPixel.y1) / (_srcRoDPixel.y2 - 1 - _srcRoDPixel.y1);
                        break;
                    }
                    case eRetimeFunctionVerticalSlit: {
                        // A horizontal ramp (alinear function of x) which is 0 at the center of the leftmost image line, and 1 at the center of the rightmost image line."
                        retimeVal = (double)(x - _srcRoDPixel.x1) / (_srcRoDPixel.x2 - 1 - _srcRoDPixel.x1);
                        break;
                    }
                    case eRetimeFunctionRetimeMap: {
                        PIX *retimePix = (PIX *)_retimeMap->getPixelAddress(x, y);
                        retimeVal = retimePix ? ((double)*retimePix / maxValue) : 0.;
                        break;
                    }
                }
                retimeVal = retimeVal * _retimeGain + _retimeOffset;
                if (!_retimeAbsolute) {
                    retimeVal += _time;
                }
                if (_filter == eFilterNearest || retimeVal == (int)retimeVal) {
                    retimeVal = std::floor(retimeVal + 0.5);
                    PIX* srcPix = (PIX*)_sourceImages->getPixelAddress(retimeVal, x, y);
                    if (srcPix) {
                        std::copy(srcPix, srcPix + nComponents, dstPix);
                    } else {
                        std::fill( dstPix, dstPix + nComponents, PIX() );
                    }
                } else {
                    PIX* fromPix = (PIX*)_sourceImages->getPixelAddress(std::floor(retimeVal), x, y);
                    PIX* toPix = (PIX*)_sourceImages->getPixelAddress(std::ceil(retimeVal), x, y);

                    float blend = retimeVal - std::floor(retimeVal);
                    float blendComp = 1.f - blend;

                    if(fromPix && toPix) {
                        for(int c = 0; c < nComponents; c++)
                            dstPix[c] = Lerp(fromPix[c], toPix[c], blend);
                    }
                    else if(fromPix) {
                        for(int c = 0; c < nComponents; c++)
                            dstPix[c] = PIX(fromPix[c] * blendComp);
                    }
                    else if(toPix) {
                        for(int c = 0; c < nComponents; c++)
                            dstPix[c] = PIX(toPix[c] * blend);
                    }
                    else {
                        std::fill( dstPix, dstPix + nComponents, PIX() );
                    }
                }

                dstPix += nComponents;
            }
        }
    }

};

/* set up and run a processor */
void
SlitScanPlugin::setupAndProcess(SlitScanProcessorBase &processor,
                                const OFX::RenderArguments &args)
{
    const double time = args.time;
    // get a dst image
    std::auto_ptr<OFX::Image>  dst( _dstClip->fetchImage(time) );

    if ( !dst.get() ) {
        OFX::throwSuiteStatusException(kOfxStatFailed);
    }
    OFX::BitDepthEnum dstBitDepth    = dst->getPixelDepth();
    OFX::PixelComponentEnum dstComponents  = dst->getPixelComponents();
    if ( ( dstBitDepth != _dstClip->getPixelDepth() ) ||
         ( dstComponents != _dstClip->getPixelComponents() ) ) {
        setPersistentMessage(OFX::Message::eMessageError, "", "OFX Host gave image with wrong depth or components");
        OFX::throwSuiteStatusException(kOfxStatFailed);
    }
    if ( (dst->getRenderScale().x != args.renderScale.x) ||
         ( dst->getRenderScale().y != args.renderScale.y) ||
         ( ( dst->getField() != OFX::eFieldNone) /* for DaVinci Resolve */ && ( dst->getField() != args.fieldToRender) ) ) {
        setPersistentMessage(OFX::Message::eMessageError, "", "OFX Host gave image with wrong scale or field properties");
        OFX::throwSuiteStatusException(kOfxStatFailed);
    }

    double retimeGain = _retimeGain->getValueAtTime(time);
    RetimeFunctionEnum retimeFunction = (RetimeFunctionEnum)_retimeFunction->getValueAtTime(time);
    FilterEnum filter = (FilterEnum)_filter->getValueAtTime(time);
    double retimeOffset = _retimeOffset->getValueAtTime(time);
    bool retimeAbsolute = _retimeAbsolute->getValueAtTime(time);

    if ( (retimeFunction == eRetimeFunctionRetimeMap) && !( _retimeMapClip && _retimeMapClip->isConnected() ) ) {
        // no retime map, equivalent to value = 0 everywhere
        retimeGain = 0.;
    }

    if (retimeGain == 0.) {
        double identityTime = retimeAbsolute ? retimeOffset : (time + retimeOffset);
        if (identityTime != (int)identityTime) {
            if (filter == eFilterNearest) {
                identityTime = std::floor(identityTime + 0.5);
            }
        }
        if (identityTime == (int)identityTime) {
            // should have been caught by isIdentity...
            std::auto_ptr<const OFX::Image> src( ( _srcClip && _srcClip->isConnected() ) ?
                                                _srcClip->fetchImage(identityTime) : 0 );
            if ( src.get() ) {
                if ( (src->getRenderScale().x != args.renderScale.x) ||
                    ( src->getRenderScale().y != args.renderScale.y) ||
                    ( ( src->getField() != OFX::eFieldNone) /* for DaVinci Resolve */ && ( src->getField() != args.fieldToRender) ) ) {
                    setPersistentMessage(OFX::Message::eMessageError, "", "OFX Host gave image with wrong scale or field properties");
                    OFX::throwSuiteStatusException(kOfxStatFailed);
                }
                OFX::BitDepthEnum srcBitDepth      = src->getPixelDepth();
                OFX::PixelComponentEnum srcComponents = src->getPixelComponents();
                if ( (srcBitDepth != dstBitDepth) || (srcComponents != dstComponents) ) {
                    OFX::throwSuiteStatusException(kOfxStatErrImageFormat);
                }
            }
            copyPixels( *this, args.renderWindow, src.get(), dst.get() );
            
            return;
        }
    }


    // set the images
    processor.setDstImg( dst.get() );

    OfxRectI srcRoDPixel = {0, 0, 0, 0};
    if (_srcClip) {
        const OfxRectD& srcRod = _srcClip->getRegionOfDefinition(time);
        OFX::Coords::toPixelEnclosing(srcRod, args.renderScale, _srcClip->getPixelAspectRatio(), &srcRoDPixel);
    }

    SourceImages sourceImages(*this, _srcClip);
    std::set<double> sourceImagesTimes;

    std::auto_ptr<const OFX::Image> retimeMap;

    switch (retimeFunction) {
        case eRetimeFunctionHorizontalSlit: {
            double a = (double)(args.renderWindow.y1 - srcRoDPixel.y1) / (srcRoDPixel.y2 - 1 - srcRoDPixel.y1);
            double b = (double)(args.renderWindow.y2 - 1 - srcRoDPixel.y1) / (srcRoDPixel.y2 - 1 - srcRoDPixel.y1);
            buildTimesSlit(time, a, b, retimeGain, retimeOffset, retimeAbsolute, filter, &sourceImagesTimes);
            break;
        }
        case eRetimeFunctionVerticalSlit: {
            double a = (double)(args.renderWindow.x1 - srcRoDPixel.y1) / (srcRoDPixel.x2 - 1 - srcRoDPixel.x1);
            double b = (double)(args.renderWindow.x2 - 1 - srcRoDPixel.x1) / (srcRoDPixel.x2 - 1 - srcRoDPixel.x1);
            buildTimesSlit(time, a, b, retimeGain, retimeOffset, retimeAbsolute, filter, &sourceImagesTimes);
            break;
        }
        case eRetimeFunctionRetimeMap: {
            if ( retimeGain != 0. && _retimeMapClip && _retimeMapClip->isConnected() ) {
                retimeMap.reset( _retimeMapClip->fetchImage(time) );
            }
            if ( !retimeMap.get() ) {
                // empty retimeMap or no gain, we need only one or two images
                double identityTime = retimeAbsolute ? retimeOffset : (time + retimeOffset);
                if (identityTime == (int)identityTime) {
                    sourceImagesTimes.insert(identityTime);
                } else {
                    if (filter == eFilterNearest) {
                        sourceImagesTimes.insert( std::floor(identityTime + 0.5) );
                    } else {
                        sourceImagesTimes.insert( std::floor(identityTime) );
                        sourceImagesTimes.insert( std::ceil(identityTime) );
                    }
                }
            } else {
                retimeMap.reset( _retimeMapClip->fetchImage(time) );
                assert(retimeMap->getPixelComponents() == ePixelComponentAlpha);
                processor.setRetimeMap(retimeMap.get());

                // scan the renderWindow in map, and gather the necessary image ids
                // (could be done in a processor)
                BitDepthEnum retimeMapDepth = retimeMap->getPixelDepth();
                switch (retimeMapDepth) {
                    case eBitDepthUByte:
                        buildTimes<unsigned char, 255>(*this,
                                                       retimeMap.get(),
                                                       time,
                                                       args.renderWindow,
                                                       retimeGain,
                                                       retimeOffset,
                                                       retimeAbsolute,
                                                       filter,
                                                       &sourceImagesTimes);
                        break;
                    case eBitDepthUShort:
                        buildTimes<unsigned short, 65535>(*this,
                                                          retimeMap.get(),
                                                          time,
                                                          args.renderWindow,
                                                          retimeGain,
                                                          retimeOffset,
                                                          retimeAbsolute,
                                                          filter,
                                                          &sourceImagesTimes);
                        break;
                    case eBitDepthFloat:
                        buildTimes<float, 1>(*this,
                                             retimeMap.get(),
                                             time,
                                             args.renderWindow,
                                             retimeGain,
                                             retimeOffset,
                                             retimeAbsolute,
                                             filter,
                                             &sourceImagesTimes);
                        break;
                    default:
                        setPersistentMessage(OFX::Message::eMessageError, "", "OFX Host gave image with wrong depth or components");
                        OFX::throwSuiteStatusException(kOfxStatFailed);
                        break;
                }
            }
            break;
        }
    }
    if ( abort() ) {
        return;
    }
    sourceImages.fetchSet(sourceImagesTimes);
    if ( abort() ) {
        return;
    }
    // set the render window
    processor.setRenderWindow(args.renderWindow);

    // set the blend between
    processor.setSourceImages(&sourceImages);


    processor.setValues(time, filter, retimeFunction, retimeGain, retimeOffset, retimeAbsolute, srcRoDPixel);

    // Call the base class process member, this will call the derived templated process code
    processor.process();
} // SlitScanPlugin::setupAndProcessSlitScanLinear

template <int nComponents>
void
SlitScanPlugin::renderForComponents(const OFX::RenderArguments &args)
{
    OFX::BitDepthEnum dstBitDepth    = _dstClip->getPixelDepth();
    switch (dstBitDepth) {
        case OFX::eBitDepthUByte: {
            SlitScanProcessor<unsigned char, nComponents, 255> fred(*this);
            setupAndProcess(fred, args);
            break;
        }
        case OFX::eBitDepthUShort: {
            SlitScanProcessor<unsigned short, nComponents, 65535> fred(*this);
            setupAndProcess(fred, args);
            break;
        }
        case OFX::eBitDepthFloat: {
            SlitScanProcessor<float, nComponents, 1> fred(*this);
            setupAndProcess(fred, args);
            break;
        }
        default:
            OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
    }
}

// the overridden render function
void
SlitScanPlugin::render(const OFX::RenderArguments &args)
{
    // instantiate the render code based on the pixel depth of the dst clip
    OFX::PixelComponentEnum dstComponents  = _dstClip->getPixelComponents();

    assert( kSupportsMultipleClipPARs   || !_srcClip || _srcClip->getPixelAspectRatio() == _dstClip->getPixelAspectRatio() );
    assert( kSupportsMultipleClipDepths || !_srcClip || _srcClip->getPixelDepth()       == _dstClip->getPixelDepth() );

    // do the rendering
    if (dstComponents == OFX::ePixelComponentRGBA) {
        renderForComponents<4>(args);
    } else if (dstComponents == OFX::ePixelComponentRGB) {
        renderForComponents<3>(args);
    } else if (dstComponents == OFX::ePixelComponentXY) {
        renderForComponents<2>(args);
    } else if (dstComponents == OFX::ePixelComponentAlpha) {
        renderForComponents<1>(args);
    } else {
        OFX::throwSuiteStatusException(kOfxStatFailed);
    }
}

mDeclarePluginFactory(SlitScanPluginFactory,; , {});
void
SlitScanPluginFactory::load()
{
    // we can't be used on hosts that don't perfrom temporal clip access
    if (!getImageEffectHostDescription()->temporalClipAccess) {
        throw OFX::Exception::HostInadequate("Need random temporal image access to work");
    }
}

/** @brief The basic describe function, passed a plugin descriptor */
void
SlitScanPluginFactory::describe(OFX::ImageEffectDescriptor &desc)
{
    // basic labels
    desc.setLabel(kPluginName);
    desc.setPluginGrouping(kPluginGrouping);
    desc.setPluginDescription(kPluginDescription);

    desc.addSupportedContext(OFX::eContextFilter);
    desc.addSupportedContext(OFX::eContextGeneral);

    // Add supported pixel depths
    desc.addSupportedBitDepth(eBitDepthUByte);
    desc.addSupportedBitDepth(eBitDepthUShort);
    desc.addSupportedBitDepth(eBitDepthFloat);

    // set a few flags
    desc.setSingleInstance(false);
    desc.setHostFrameThreading(true); // specific to SlitScan: host frame threading helps us: we require less input images to render a small area
    desc.setSupportsMultiResolution(kSupportsMultiResolution);
    desc.setSupportsTiles(kSupportsTiles);
    desc.setTemporalClipAccess(true); // say we will be doing random time access on clips
    desc.setRenderTwiceAlways(true); // each field has to be rendered separately, since it may come from a different time
    desc.setSupportsMultipleClipPARs(kSupportsMultipleClipPARs);
    desc.setSupportsMultipleClipDepths(kSupportsMultipleClipDepths);
    desc.setRenderThreadSafety(kRenderThreadSafety);

    // we can't be used on hosts that don't perfrom temporal clip access
    if (!getImageEffectHostDescription()->temporalClipAccess) {
        throw OFX::Exception::HostInadequate("Need random temporal image access to work");
    }
#ifdef OFX_EXTENSIONS_NATRON
    desc.setChannelSelector(ePixelComponentNone);
#endif
}

/** @brief The describe in context function, passed a plugin descriptor and a context */
void
SlitScanPluginFactory::describeInContext(OFX::ImageEffectDescriptor &desc,
                                         ContextEnum /*context*/)
{
    ClipDescriptor *srcClip = desc.defineClip(kOfxImageEffectSimpleSourceClipName);

    srcClip->addSupportedComponent(ePixelComponentRGBA);
    srcClip->addSupportedComponent(ePixelComponentRGB);
    srcClip->addSupportedComponent(ePixelComponentXY);
    srcClip->addSupportedComponent(ePixelComponentAlpha);
    srcClip->setTemporalClipAccess(true); // say we will be doing random time access on this clip
    srcClip->setSupportsTiles(kSupportsTiles);
    srcClip->setFieldExtraction(eFieldExtractDoubled); // which is the default anyway

    ClipDescriptor *retimeMapClip = desc.defineClip(kClipRetimeMap);
    retimeMapClip->addSupportedComponent(ePixelComponentAlpha);
    retimeMapClip->setSupportsTiles(kSupportsTiles);
    retimeMapClip->setOptional(true);
    retimeMapClip->setIsMask(false);

    // create the mandated output clip
    ClipDescriptor *dstClip = desc.defineClip(kOfxImageEffectOutputClipName);
    dstClip->addSupportedComponent(ePixelComponentRGBA);
    dstClip->addSupportedComponent(ePixelComponentRGB);
    dstClip->addSupportedComponent(ePixelComponentXY);
    dstClip->addSupportedComponent(ePixelComponentAlpha);
    dstClip->setFieldExtraction(eFieldExtractDoubled); // which is the default anyway
    dstClip->setSupportsTiles(kSupportsTiles);

    // make a page to put it in
    PageParamDescriptor *page = desc.definePageParam("Controls");

    {
        ChoiceParamDescriptor *param = desc.defineChoiceParam(kParamRetimeFunction);
        param->setLabel(kParamRetimeFunctionLabel);
        param->setHint(kParamRetimeFunctionHint);
        assert(param->getNOptions() == eRetimeFunctionHorizontalSlit);
        param->appendOption(kParamRetimeFunctionOptionHorizontalSlit, kParamRetimeFunctionOptionHorizontalSlitHint);
        assert(param->getNOptions() == eRetimeFunctionVerticalSlit);
        param->appendOption(kParamRetimeFunctionOptionVerticalSlit, kParamRetimeFunctionOptionVerticalSlitHint);
        assert(param->getNOptions() == eRetimeFunctionRetimeMap);
        param->appendOption(kParamRetimeFunctionOptionRetimeMap, kParamRetimeFunctionOptionRetimeMapHint);
        param->setDefault( (int)kParamRetimeFunctionDefault );
        if (page) {
            page->addChild(*param);
        }
    }

    {
        DoubleParamDescriptor *param = desc.defineDoubleParam(kParamRetimeOffset);
        param->setDefault(kParamRetimeOffsetDefault);
        param->setHint(kParamRetimeOffsetHint);
        param->setLabel(kParamRetimeOffsetLabel);
        param->setAnimates(true);
        if (page) {
            page->addChild(*param);
        }
    }
    {
        DoubleParamDescriptor *param = desc.defineDoubleParam(kParamRetimeGain);
        param->setDefault(kParamRetimeGainDefault);
        param->setHint(kParamRetimeGainHint);
        param->setLabel(kParamRetimeGainLabel);
        param->setAnimates(true);
        if (page) {
            page->addChild(*param);
        }
    }
    {
        BooleanParamDescriptor *param = desc.defineBooleanParam(kParamRetimeAbsolute);
        param->setDefault(kParamRetimeAbsoluteDefault);
        param->setHint(kParamRetimeAbsoluteHint);
        param->setLabel(kParamRetimeAbsoluteLabel);
        if (page) {
            page->addChild(*param);
        }
    }
    {
        Int2DParamDescriptor *param = desc.defineInt2DParam(kParamFrameRange);
        param->setDefault(kParamFrameRangeDefault);
        param->setHint(kParamFrameRangeHint);
        param->setLabel(kParamFrameRangeLabel);
        param->setDimensionLabels("min", "max");
        if (page) {
            page->addChild(*param);
        }
    }

    {
        ChoiceParamDescriptor *param = desc.defineChoiceParam(kParamFilter);
        param->setLabel(kParamFilterLabel);
        param->setHint(kParamFilterHint);
        assert(param->getNOptions() == eFilterNearest);
        param->appendOption(kParamFilterOptionNearest, kParamFilterOptionNearestHint);
        assert(param->getNOptions() == eFilterLinear);
        param->appendOption(kParamFilterOptionLinear, kParamFilterOptionLinearHint);
        //assert(param->getNOptions() == eFilterBox);
        //param->appendOption(kParamFilterOptionBox, kParamFilterOptionBoxHint);
        param->setDefault( (int)kParamFilterDefault );
        if (page) {
            page->addChild(*param);
        }
    }
} // SlitScanPluginFactory::describeInContext

/** @brief The create instance function, the plugin must return an object derived from the \ref OFX::ImageEffect class */
ImageEffect*
SlitScanPluginFactory::createInstance(OfxImageEffectHandle handle,
                                      ContextEnum /*context*/)
{
    return new SlitScanPlugin(handle);
}

static SlitScanPluginFactory p(kPluginIdentifier, kPluginVersionMajor, kPluginVersionMinor);
mRegisterPluginFactoryInstance(p)

OFXS_NAMESPACE_ANONYMOUS_EXIT

