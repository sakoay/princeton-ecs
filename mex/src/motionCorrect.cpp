/**
  Computes a motion corrected version of the given movie using cv::matchTemplate().

  Usage syntax:
    mc  = cv.motionCorrect( inputPath, maxShift, maxIter                                  ...
                          , [displayProgress = false], [stopBelowShift = 0]               ...
                          , [blackTolerance = nan], [medianRebin = 1]                     ...
                          , [frameSkip = [0 0]], [centerShifts = ~isnan(blackTolerance)]  ...
                          , [preferSmallestShifts = false]                                ...
                          , [methodInterp = cve.InterpolationFlags.INTER_LINEAR]          ...
                          , [methodCorr = cve.TemplateMatchModes.TM_CCOEFF_NORMED]        ...
                          , [emptyValue = mean]                                           ...
                          );
    mc  = cv.motionCorrect( {input, template}, ... );

  The median image is used as the template to which frames are aligned, except for 
  a border of maxShift pixels in size which is omitted since it is possible for 
  motion correction to crop up to that much of the frame.

  The medianRebin parameter can be used to specify that the median should be computed 
  using this number of frames per data point, instead of all frames. This can help
  reduce the amount of time required to motion correct, and also to obtain a sensible 
  template for data that is very noisy or close to zero per frame.

  The frameSkip parameter allows one to subsample the input movie in terms of frames.
  It should be provided as a pair [offset, skip] where offset is the first frames to
  skip, and skip is the number of frames to skip between reads. For example, 
  frameSkip = [1 1] will start reading from the *second* frame and skip every other 
  frame, i.e. read all even frames for motion correction. The produced shifts will
  thus be fewer than the full movie and equal to the number of subsampled frames.

  Author:   Sue Ann Koay (koay@princeton.edu)
*/


#include <cmath>
#include <vector>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/highgui.hpp>
#include <mex.h>
#include "lib/matUtils.h"
#include "lib/imageStatistics.h"
#include "lib/manipulateImage.h"
#include "lib/conversionUtils.h"
#include "lib/cvToMatlab.h"




double harmonicMean(double Fbound, double Fmean, double Fdev)
{
  const double        Frange    = (Fbound - Fmean);
  const double        Festim    = 2/(1/std::abs(Frange) + 1/std::abs(Fdev));
  return Fmean + (Fdev < 0 ? -1 : 1) * Festim;
}


static const char*    METHOD_INTERP[] = { "nearestNeighbor"
                                        , "linear"
                                        , "cubic"
                                        , "area"
                                        , "lanczos4"
                                        };

static const char*    METHOD_CORR[]   = { "squaredDifference"
                                        , "sqDiffNormed"
                                        , "crossCorrelation"
                                        , "crossCorrNormed"
                                        , "correlationCoeff"
                                        , "corrCoeffNormed"
                                        };



#ifndef __OPENCV_HACK_SAK__
template<typename Pixel, typename SignedPix>
struct GetSignedUnsignedRange {
  void operator()(const cv::Mat& image, double& signedRange, double& unsignedRange) 
  {
    // Loop over each pixel in the image 
    double            minUnsigned = std::numeric_limits<double>::max();
    double            maxUnsigned = std::numeric_limits<double>::lowest();
    double            minSigned   = std::numeric_limits<double>::max();
    double            maxSigned   = std::numeric_limits<double>::lowest();
    for (int iRow = 0; iRow < image.rows; ++iRow) {
      const Pixel*    pixRow      = image.ptr<Pixel>(iRow);
      for (int iCol = 0; iCol < image.cols; ++iCol) {
        double        unsignedPix = pixRow[iCol];
        minUnsigned   = std::min(minUnsigned, unsignedPix);
        maxUnsigned   = std::max(maxUnsigned, unsignedPix);

        double        signedPix   = static_cast<SignedPix>(pixRow[iCol]);
        minSigned     = std::min(minSigned, signedPix);
        maxSigned     = std::max(maxSigned, signedPix);
      } // end loop over columns
    } // end loop over rows

    unsignedRange     = maxUnsigned - minUnsigned;
    signedRange       = maxSigned   - minSigned  ;
  }
};


void typecastCVData(std::vector<cv::Mat>& imgStack, std::vector<cv::Mat>& origStack, int targetType)
{
  origStack.swap(imgStack);
  imgStack.reserve(origStack.size());

  for (size_t iFrame = 0; iFrame < origStack.size(); ++iFrame) {
    imgStack.push_back(cv::Mat(origStack[iFrame].size(), targetType, (void*) origStack[iFrame].data));
  }
}
#endif //__OPENCV_HACK_SAK__



typedef   bool (*Comparator)(float, float);
bool lessThan   (float a, float b) { return a < b; }
bool greaterThan(float a, float b) { return a > b; }

void findLocalOptimum(const cv::Mat& metric, const std::vector<double>& radius2, cv::Point& optimum, Comparator reject)
{
  const int             lastRow         = metric.rows - 1;
  const int             lastCol         = metric.cols - 1;
  double                bestRadius2     = radius2[ optimum.x + metric.cols*optimum.y ];

  for (int iY = 1, index = metric.cols; iY < lastRow; ++iY) {
    // The following are the three rows centered at the test row
    const float*        row0            = metric.ptr<float>(iY - 1);
    const float*        row1            = metric.ptr<float>(iY    );
    const float*        row2            = metric.ptr<float>(iY + 1);

    ++index;            // skipping first column
    for (int iX = 1; iX < lastCol; ++iX, ++index) {
      if  ( reject( row1[iX], row0[iX  ] )        // N
         || reject( row1[iX], row2[iX  ] )        // S
         || reject( row1[iX], row1[iX+1] )        // E
         || reject( row1[iX], row1[iX-1] )        // W
         || reject( row1[iX], row0[iX+1] )        // NE
         || reject( row1[iX], row0[iX-1] )        // NW
         || reject( row1[iX], row2[iX+1] )        // SE
         || reject( row1[iX], row2[iX-1] )        // SW
         || bestRadius2 < radius2[index]
          )
        continue;

      bestRadius2       = radius2[index];
      optimum.x         = iX;
      optimum.y         = iY;
    }
    ++index;            // skipping last column
  }
}



///////////////////////////////////////////////////////////////////////////
// Main entry point to a MEX function
///////////////////////////////////////////////////////////////////////////


void mexFunction(int nlhs, mxArray *plhs[], int nrhs, const mxArray *prhs[])
{  
  // Check inputs to mex function
  if (nrhs < 3 || nrhs > 13 || nlhs < 1 || nlhs > 2) {
    mexEvalString("help cv.motionCorrect");
    mexErrMsgIdAndTxt( "motionCorrect:usage", "Incorrect number of inputs/outputs provided." );
  }


  // Parse input
  const mxArray*              input           = prhs[0];
  const int                   maxShift        = int( mxGetScalar(prhs[1]) );
  const int                   maxIter         = int( mxGetScalar(prhs[2]) );
  const bool                  displayProgress = ( nrhs >  3 ? mxGetScalar(prhs[3]) > 0     : false  );
  const double                stopBelowShift  = ( nrhs >  4 ? mxGetScalar(prhs[4])         : 0.     );
  const double                emptyProb       = ( nrhs >  5 ? mxGetScalar(prhs[5])         : -999.  );
  double*                     usrBlackValue   = ( nrhs >  5 && mxGetNumberOfElements(prhs[5]) > 1 ? mxGetPr(prhs[5]) : 0 );
  int                         medianRebin     = ( nrhs >  6 ? int( mxGetScalar(prhs[6]) )  : 1      );
  const mxArray*              frameSkip       = ( nrhs >  7 ? prhs[7]                      : 0      );
  bool                        centerShifts    = ( nrhs >  8 ? mxGetScalar(prhs[8]) > 0     : !(emptyProb > 0) );
  const bool                  preferSmallest  = ( nrhs >  9 ? mxGetScalar(prhs[9]) > 0     : false  );
  const int                   methodInterp    = ( nrhs > 10 ? int( mxGetScalar(prhs[10]))  : cv::InterpolationFlags::INTER_LINEAR     );
  const int                   methodCorr      = ( nrhs > 11 ? int( mxGetScalar(prhs[11]))  : cv::TemplateMatchModes::TM_CCOEFF_NORMED );
  const double                usrEmptyValue   = ( nrhs > 12 ?      mxGetScalar(prhs[12])   : 0.     );
  const bool                  emptyIsMean     = ( nrhs <=13 );
  const bool                  subPixelReg     = ( methodInterp >= 0 );

  
  //---------------------------------------------------------------------------

  std::vector<cv::Mat>        imgStack, refStack;

  // If a template is explicitly provided, use that
  if (mxIsCell(input)) {
    if (mxGetNumberOfElements(input) != 2)
      mexErrMsgIdAndTxt( "motionCorrect:input", "If input is a cell array, it must be of the form {input,template}." );
    
    const mxArray*            matTemplate     = mxGetCell(input, 1);
    if (!mxIsNumeric(matTemplate) || mxIsComplex(matTemplate))
      mexErrMsgIdAndTxt( "motionCorrect:template", "template must be a numeric matrix (image)." );
    cvMatlabCall<MatlabToCVMat>(refStack, mxGetClassID(matTemplate), matTemplate);

    input                     = mxGetCell(input, 0);
    centerShifts              = false;        // Don't center if template is explicitly provided
  }

  // Frame skipping if so desired
  int                         firstFrame      = 0;
  int                         skipFrames      = 0;
  if (frameSkip) {
    if (mxGetNumberOfElements(frameSkip) != 2)
      mexErrMsgIdAndTxt( "motionCorrect:arguments", "frameSkip must be a 2-element array [offset, skip]." );
    const double*             skip            = mxGetPr(frameSkip);
    firstFrame                = cv::saturate_cast<int>( skip[0] );
    skipFrames                = cv::saturate_cast<int>( skip[1] );
  }


  // If a matrix is directly provided, need to copy it into OpenCV format
  char*                       inputPath       = ( mxIsChar(input) 
                                               && mxGetNumberOfDimensions(input) < 3 
                                               && ( mxGetN(input) == 1 || mxGetM(prhs[1]) == 1 )
                                                ? mxArrayToString(input) 
                                                : 0 
                                                );
  if (!inputPath)
    cvMatlabCall<MatlabToCVMat>(imgStack, mxGetClassID(input), input, firstFrame, skipFrames);

  // Otherwise load image with stored bit depth
#ifdef __OPENCV_HACK_SAK__
  else if (!cv::imreadmulti(inputPath, imgStack, cv::ImreadModes::IMREAD_UNCHANGED, firstFrame, skipFrames))
      mexErrMsgIdAndTxt( "motionCorrect:load", "Failed to load input image." );

#else
  else if (!cv::imreadmulti(inputPath, imgStack, cv::ImreadModes::IMREAD_UNCHANGED))
      mexErrMsgIdAndTxt( "motionCorrect:load", "Failed to load input image." );

  // Inefficient frame skipping if we can't hack OpenCV
  else if (frameSkip) {
    int                       iOutput         = 0;
    for (int iInput = firstFrame; iInput < imgStack.size(); iInput += 1 + skipFrames, ++iOutput)
      imgStack[iOutput]       = imgStack[iInput];
    imgStack.resize(iOutput);
  }
#endif //__OPENCV_HACK_SAK__



  // Sanity checks on image stack
  if (imgStack.empty())
    mexErrMsgIdAndTxt( "motionCorrect:load", "Input image has no frames." );
  if (imgStack[0].cols * imgStack[0].rows < 3)
    mexErrMsgIdAndTxt( "motionCorrect:load", "Input image too small, must have at least 3 pixels." );

  // The frame rebinning factor (for computation of median only) must be a divisor of
  // the number of frames to avoid edge artifacts
  const size_t                numFrames       = imgStack.size();
  size_t                      numMedian       = static_cast<size_t>(std::ceil( 1.0 * imgStack.size() / medianRebin ));



  // The template size restricts the maximum allowable shift
  const int                   firstRefRow     = std::min(maxShift, (imgStack[0].rows - 1)/2);
  const int                   firstRefCol     = std::min(maxShift, (imgStack[0].cols - 1)/2);
  const size_t                metricSize[]    = {size_t(2*firstRefRow + 1), size_t(2*firstRefCol + 1), numFrames};
  const int                   metricOffset    = static_cast<int>( metricSize[0] * metricSize[1] );

  // If so desired, omit black (empty) frames
  std::vector<bool>           isEmpty;
  double                      blackValue      = mxGetNaN();
  if (emptyProb > 0) {
    if (usrBlackValue)        blackValue      = usrBlackValue[1];
    double*                   ptrZeroValue    = &blackValue;
    cvCall<DetectEmptyFrames>(imgStack, isEmpty, emptyProb, ptrZeroValue);
  }
  else isEmpty.assign(imgStack.size(), false);


#ifndef __OPENCV_HACK_SAK__
  //...........................................................................
  // HACK to fix intrinsic OpenCV problem where signed integer data is loaded as unsigned 
  // -- detect this condition by seeing if the data range is much more compact when 
  // converted to signed integers
  std::vector<cv::Mat>        origStack;
  if (inputPath) {
    for (int iFrame = 0; iFrame < imgStack.size(); ++iFrame) {
      if (isEmpty[iFrame])    continue;

      // Only consider unsigned integer types
      double                  signedRange, unsignedRange;
      switch (imgStack[iFrame].depth()) {
      case CV_8U :
        GetSignedUnsignedRange<uchar, schar>()(imgStack[iFrame], signedRange, unsignedRange);
        if (signedRange < 0.5 * unsignedRange) {
          mexWarnMsgIdAndTxt("motionCorrect:signedData", "Guessed that data is signed 8-bit based on signed range = %.5g vs. unsigned range = %.5g: %s", signedRange, unsignedRange, inputPath);
          typecastCVData(imgStack, origStack, CV_8S);
        }
        break;
      case CV_16U:
        GetSignedUnsignedRange<ushort, short>()(imgStack[iFrame], signedRange, unsignedRange);
        if (signedRange < 0.5 * unsignedRange) {
          mexWarnMsgIdAndTxt("motionCorrect:signedData", "Guessed that data is signed 16-bit based on signed range = %.5g vs. unsigned range = %.5g: %s", signedRange, unsignedRange, inputPath);
          typecastCVData(imgStack, origStack, CV_16S);
        }
        break;
      }

      break;
    }
  }
  //...........................................................................
#endif //__OPENCV_HACK_SAK__




  // Create output structure
  mxArray*                    outXShifts      = mxCreateDoubleMatrix(numFrames, maxIter, mxREAL);
  mxArray*                    outYShifts      = mxCreateDoubleMatrix(numFrames, maxIter, mxREAL);
  mxArray*                    outStackMetric  = mxCreateNumericArray(3, metricSize, mxSINGLE_CLASS, mxREAL);
  mxArray*                    outOptimMetric  = mxCreateDoubleMatrix(numFrames, 1, mxREAL);
  double*                     xShifts         = mxGetPr(outXShifts);
  double*                     yShifts         = mxGetPr(outYShifts);
  float*                      stackMetric     = (float*) mxGetData(outStackMetric);
  double*                     optimMetric     = mxGetPr(outOptimMetric);

  MatToMatlab<float,float>    dataCopier;

  
  //---------------------------------------------------------------------------
  // Preallocate temporary storage for computations
  cv::Mat                     frmInput  (imgStack[0].rows, imgStack[0].cols, CV_32F);
  cv::Mat                     frmTemp   (imgStack[0].rows, imgStack[0].cols, CV_32F);
  cv::Mat                     imgRef    (imgStack[0].rows, imgStack[0].cols, CV_32F);
  cv::Mat                     metric    (metricSize[0]   , metricSize[1]   , CV_32F);
  cv::Mat                     refRegion       = imgRef(cv::Rect(firstRefCol, firstRefRow, imgStack[0].cols - 2*firstRefCol, imgStack[0].rows - 2*firstRefRow));

  std::vector<float>          traceTemp (std::max(numMedian, refStack.size()));
  std::vector<cv::Mat>        imgShifted(numMedian);
  std::vector<double>         medWeight;
  std::vector<double>         radius2;

  // Precompute squared radius of each metric pixel from the center, for finding local optima
  if (preferSmallest) {
    radius2.resize(metric.rows * metric.cols);
    for (int row = 0, index = 0; row < metric.rows; ++row) {
      const double            dRow2           = sqr( row - 0.5*metric.rows );
      for (int col = 0; col < metric.cols; ++col, ++index)
        radius2[index]        = dRow2 + sqr( col - 0.5*metric.cols );
    }
  }


  // Translation matrix, for use with sub-pixel registration
  cv::Mat                     translator(2, 3, CV_32F);
  float*                      xTrans          = translator.ptr<float>(0);
  float*                      yTrans          = translator.ptr<float>(1);
  translator                  = cv::Scalar(0);
  xTrans[0]                   = 1;
  yTrans[1]                   = 1;


  // Copy frames to temporary storage with the appropriate resolution
  medWeight.assign(numMedian, 1.);
  for (size_t iMedian = 0, iFrame = 0; iMedian < numMedian; ++iMedian) {
    int                       count           = 0;
    imgShifted[iMedian].create(imgStack[0].rows, imgStack[0].cols, CV_32F);
    imgShifted[iMedian]       = cv::Scalar(0);
    for (int iBin = 0; iBin < medianRebin && iFrame < numFrames; ++iBin, ++iFrame) {
      if (isEmpty[iFrame])    continue;
      cvCall<AddImage32>(imgStack[iFrame], imgShifted[iMedian]);
      ++count;
    }
    if (count)
      medWeight[iMedian]      = 1. / count;
    else 
      imgShifted[iMedian]     = cv::Scalar(mxGetNaN());
  }

                                                                            
  // Obtain some global statistics to be used for data scaling and display
  double                      showMin, showMax, templateMin, templateMax;
  SampleStatistics            inputStats;
  if (displayProgress || emptyIsMean)
  {
    cvCall<AccumulateMatStatistics>(imgStack, inputStats);
    if (inputStats.getMaximum() <= inputStats.getMinimum())
      mexErrMsgIdAndTxt( "motionCorrect:image", "Invalid range [%.3g, %.3g] of pixel values in image stack; the image cannot be completely uniform for motion correction.", inputStats.getMaximum(), inputStats.getMinimum());
    double                    stdDev          = inputStats.getRMS();
    showMin                   = std::max(inputStats.getMinimum(), harmonicMean(inputStats.getMinimum(), inputStats.getMean(), -1*stdDev));
    showMax                   = std::min(inputStats.getMaximum(), harmonicMean(inputStats.getMaximum(), inputStats.getMean(), +4*stdDev));

    SampleStatistics          medianStats;
    std::vector<double>*      weightPtr       = &medWeight;
    cvCall<AccumulateMatStatistics>(imgShifted, medianStats, weightPtr);
    stdDev                    = medianStats.getRMS();
    templateMin               = std::max(medianStats.getMinimum(), harmonicMean(medianStats.getMinimum(), medianStats.getMean(), -2*stdDev));
    templateMax               = std::min(medianStats.getMaximum(), harmonicMean(medianStats.getMaximum(), medianStats.getMean(), +5*stdDev));
  }
  
  const cv::Scalar            emptyValue( emptyIsMean ? inputStats.getMean() : usrEmptyValue );
  char                        strTemplate[1000];
  
  if (displayProgress) {
    cv::namedWindow("Corrected", CV_WINDOW_NORMAL | CV_WINDOW_KEEPRATIO | CV_GUI_EXPANDED);
    cv::resizeWindow("Corrected", imgStack[0].cols, imgStack[0].rows);
  }

  
  //---------------------------------------------------------------------------

  const bool                  useMinimum      = (methodCorr == cv::TemplateMatchModes::TM_SQDIFF || methodCorr == cv::TemplateMatchModes::TM_SQDIFF_NORMED);
  Comparator                  optimReject     = useMinimum ? greaterThan : lessThan;
  int                         iteration       = 0;
  double                      midXShift       = 0;
  double                      midYShift       = 0;
  double                      maxRelShift     = 1e308;
  while (true)
  {
    // Relative index at which the previous shifts were stored
    const int                 iPrevX          = ( iteration < 1 ? 0 : static_cast<int>(numFrames) );
    const int                 iPrevY          = ( iteration < 1 ? 0 : static_cast<int>(numFrames) );

    // Compute median image 
    if (iteration > 1 || refStack.empty()) {
      // Scale to compensate for black (omitted) frames
      for (size_t iMedian = 0; iMedian < numMedian; ++iMedian)
        imgShifted[iMedian]  *= medWeight[iMedian];

      // Translate reference image so as to waste as few pixels as possible
      if (midXShift != 0 || midYShift != 0) {
        cvCall<MedianVecMat32>(imgShifted, frmTemp, traceTemp);
        if (subPixelReg) {
          xTrans[2]           = static_cast<float>( -midXShift );
          yTrans[2]           = static_cast<float>( -midYShift );
          cv::warpAffine( frmTemp, imgRef, translator, imgRef.size()
                        , methodInterp, cv::BorderTypes::BORDER_CONSTANT, emptyValue
                        );
        }
        else  cvCall<CopyShiftedImage32>(imgRef, frmTemp, midYShift, midXShift, emptyValue[0]);
      }
      else    cvCall<MedianVecMat32>(imgShifted, imgRef, traceTemp);
    }
    else      cvCall<MedianVecMat32>(refStack, imgRef, traceTemp  /*, firstRefRow, firstRefCol ????*/);


    // Stop if the maximum shift relative to the previous iteration is small enough
    if (maxRelShift < stopBelowShift)         break;
    if (iteration >= maxIter)                 break;
    ++iteration;

    if (displayProgress && (iteration == 1 || refStack.empty())) {
      //imshoweq("Template", imgRef, -minValue);
      if (refStack.empty())   std::sprintf(strTemplate, "Template (iteration %d) : %d-frame median", iteration, medianRebin);
      else                    std::sprintf(strTemplate, "Template (user provided)");
      cv::namedWindow(strTemplate, CV_WINDOW_NORMAL | CV_WINDOW_KEEPRATIO | CV_GUI_EXPANDED);
      cv::resizeWindow(strTemplate, imgStack[0].cols, imgStack[0].rows);
      imshowrange(strTemplate, imgRef, templateMin, templateMax);
      mexEvalString("drawnow");
    }


    //.........................................................................

    // Loop through frames and correct each one
    float*                    ptrMetric       = stackMetric;
    double                    minXShift       = 1e308, maxXShift = -1e308;
    double                    minYShift       = 1e308, maxYShift = -1e308;
    maxRelShift               = -1e308;
    for (size_t iFrame = 0, iMedian = 0, iBin = 0, isFirst = true; iFrame < numFrames; ++iFrame) 
    {
      // Enforce zero shift for black frames
      if (isEmpty[iFrame]) {
        if (++iBin >= medianRebin) {
          iBin                = 0;
          isFirst             = true;
          ++iMedian;
        }
        continue;
      }


      imgStack[iFrame].convertTo(frmInput, CV_32F);
      //if (displayProgress)    imshowrange("Image", frmInput, showMin, showMax);


      // Obtain metric values for all possible shifts and find the optimum
      cv::Point               optimum;
      cv::matchTemplate(frmInput, refRegion, metric, methodCorr);
      if (useMinimum)         cv::minMaxLoc(metric, optimMetric + iFrame, NULL, &optimum, NULL    );
      else                    cv::minMaxLoc(metric, NULL, optimMetric + iFrame, NULL    , &optimum);
      if (preferSmallest)     // This is an additional call so that we default to the global optimum
        findLocalOptimum(metric, radius2, optimum, optimReject);
      dataCopier(metric, CV_32F, ptrMetric);
      ptrMetric              += metricOffset;


      // If interpolation is desired, use a gaussian peak fit to resolve it
      cv::Mat&                frmShifted      = ( medianRebin > 1 ? frmTemp : imgShifted[iMedian] );
      double                  colShift, rowShift;
      if (subPixelReg) {

        // The following are the three rows centered at the optimum
        const float*          row0            = optimum.y > 0             ? metric.ptr<float>(optimum.y - 1) : 0;
        const float*          row1            =                             metric.ptr<float>(optimum.y    )    ;
        const float*          row2            = optimum.y < metric.rows-1 ? metric.ptr<float>(optimum.y + 1) : 0;
        
        // Precompute the log value once and for all
        const double          ln10            = optimum.x > 0             ? log(row1[optimum.x - 1]) : mxGetNaN();
        const double          ln11            =                             log(row1[optimum.x    ])             ;
        const double          ln12            = optimum.x < metric.cols-1 ? log(row1[optimum.x + 1]) : mxGetNaN();
        const double          ln01            = row0                      ? log(row0[optimum.x    ]) : mxGetNaN();
        const double          ln21            = row2                      ? log(row2[optimum.x    ]) : mxGetNaN();
        
        // 1D Gaussian interpolation in each direction
        double                xPeak           = ( ln10 - ln12 ) / ( 2 * ln10 - 4 * ln11 + 2 * ln12 );
        double                yPeak           = ( ln01 - ln21 ) / ( 2 * ln01 - 4 * ln11 + 2 * ln21 );
        if (xPeak != xPeak)   xPeak           = 0;
        if (yPeak != yPeak)   yPeak           = 0;
        xTrans[2]             = colShift      = -( optimum.x - firstRefCol + xPeak );
        yTrans[2]             = rowShift      = -( optimum.y - firstRefRow + yPeak );

        // Perform an affine transformation i.e. sub-pixel shift via interpolation
        cv::warpAffine( frmInput, frmShifted, translator, frmShifted.size()
                      , methodInterp, cv::BorderTypes::BORDER_CONSTANT, emptyValue
                      );
      }

      // In case of no sub-pixel interpolation, perform a simple (and fast) pixel shift
      else {
        // Remember that the template is offset so shifts are relative to that
        colShift              = -( optimum.x - firstRefCol );
        rowShift              = -( optimum.y - firstRefRow );
        cvCall<CopyShiftedImage32>(frmShifted, frmInput, rowShift, colShift, emptyValue[0]);
      }

      // Record history of shifts
      maxRelShift             = std::max(maxRelShift, std::fabs(colShift - xShifts[iFrame - iPrevX]));
      maxRelShift             = std::max(maxRelShift, std::fabs(rowShift - yShifts[iFrame - iPrevY]));
      xShifts[iFrame]         = colShift;
      yShifts[iFrame]         = rowShift;
      minXShift               = std::min(minXShift, colShift);
      minYShift               = std::min(minYShift, rowShift);
      maxXShift               = std::max(maxXShift, colShift);
      maxYShift               = std::max(maxYShift, rowShift);


      if (displayProgress) {
        //imshoweq("Corrected", frmShifted, -minValue);
        imshowrange("Corrected", frmShifted, showMin, showMax);
        mexEvalString("drawnow");
      }


      // Aggregate frames for median computation if so requested
      if (iMedian >= numMedian)
        mexErrMsgIdAndTxt( "motionCorrect:sanity", "Invalid median bin %d >= %d, should not be possible.", iMedian, numMedian);
      if (isFirst) {
        isFirst               = false;
        frmShifted.copyTo(imgShifted[iMedian]);
      }
      else 
        imgShifted[iMedian] += frmShifted;
      if (++iBin >= medianRebin) {
        iBin                  = 0;
        isFirst               = true;
        ++iMedian;
      }
    } // end loop over frames


    // Adjust shifts so that they span the range symmetrically
    if (centerShifts) {
      midXShift               = (minXShift + maxXShift) / 2;
      midYShift               = (minYShift + maxYShift) / 2;
      for (size_t iFrame = 0; iFrame < numFrames; ++iFrame) {
        xShifts[iFrame]      -= midXShift;
        yShifts[iFrame]      -= midYShift;
      }
    }
    xShifts                  += numFrames;
    yShifts                  += numFrames;
  } // end loop over iterations
  if (displayProgress)        cv::destroyWindow("Corrected");



  //---------------------------------------------------------------------------
  // Output

  // Truncate shift arrays in case iterations are stopped before the max
  if (iteration < maxIter) {
    mxSetN(outXShifts, iteration);
    mxSetN(outYShifts, iteration);
  }


  // Compute median image one final time to store the reference image
  //cvCall<MedianVecMat32>(imgShifted, imgRef, traceTemp, firstRefRow, firstRefCol);
  mxArray*                    outRef          = mxCreateNumericMatrix(imgRef.rows, imgRef.cols, mxSINGLE_CLASS, mxREAL);
  float*                      ptrRef          = (float*) mxGetData(outRef);
  cvMatlabCall<MatToMatlab>(imgRef, mxGetClassID(outRef), ptrRef);

  // Black frames detection parameters
  mxArray*                    outBlackTol     = mxCreateDoubleMatrix(1, 2, mxREAL);
  double*                     ptrBlackTol     = mxGetPr(outBlackTol);
  ptrBlackTol[0]              = emptyProb;
  ptrBlackTol[1]              = blackValue;

  // Parameters
  static const char*          PARAM_FIELDS[]  = { "maxShift"
                                                , "maxIter"
                                                , "stopBelowShift"
                                                , "blackTolerance"
                                                , "medianRebin"
                                                , "frameSkip"
                                                , "interpolation"
                                                , "emptyValue"
                                                };
  mxArray*                    outParams       = mxCreateStructMatrix(1, 1, 8, PARAM_FIELDS);
  mxSetField(outParams, 0, "maxShift"      , mxCreateDoubleScalar(maxShift));
  mxSetField(outParams, 0, "maxIter"       , mxCreateDoubleScalar(maxIter));
  mxSetField(outParams, 0, "stopBelowShift", mxCreateDoubleScalar(stopBelowShift));
  mxSetField(outParams, 0, "blackTolerance", outBlackTol);
  mxSetField(outParams, 0, "medianRebin"   , mxCreateDoubleScalar(medianRebin));
  if (frameSkip)              mxSetField(outParams, 0, "frameSkip", mxDuplicateArray(frameSkip));
  else                        mxSetField(outParams, 0, "frameSkip", mxCreateDoubleMatrix(0, 0, mxREAL));
  mxSetField(outParams, 0, "interpolation" , mxCreateString(METHOD_INTERP[methodInterp % 5]));      // HACK: ignore flags
  mxSetField(outParams, 0, "emptyValue"    , mxCreateDoubleScalar(emptyValue[0]));

  // Metric
  static const char*          METRIC_FIELDS[] = { "name"
                                                , "values"
                                                , "optimum"
                                                };
  mxArray*                    outMetric       = mxCreateStructMatrix(1, 1, 3, METRIC_FIELDS);
  mxSetField(outMetric, 0, "name"       , mxCreateString(METHOD_CORR[methodCorr]));
  mxSetField(outMetric, 0, "values"     , outStackMetric);
  mxSetField(outMetric, 0, "optimum"    , outOptimMetric);

  // Motion correction data structure
  static const char*          OUT_FIELDS[]    = { "xShifts"
                                                , "yShifts"
                                                , "inputSize"
                                                , "method"
                                                , "params"
                                                , "metric"
                                                , "reference"
                                                };
  plhs[0]                     = mxCreateStructMatrix(1, 1, 7, OUT_FIELDS);


  mxArray*                    outSize         = mxCreateDoubleMatrix(1, 3, mxREAL);
  double*                     sizePtr         = mxGetPr(outSize);
  sizePtr[0]                  = imgStack[0].rows;
  sizePtr[1]                  = imgStack[0].cols;
  sizePtr[2]                  = imgStack.size();
  mxSetField(plhs[0], 0, "xShifts"  , outXShifts);
  mxSetField(plhs[0], 0, "yShifts"  , outYShifts);
  mxSetField(plhs[0], 0, "inputSize", outSize);
  mxSetField(plhs[0], 0, "method"   , mxCreateString("cv.motionCorrect"));
  mxSetField(plhs[0], 0, "params"   , outParams);
  mxSetField(plhs[0], 0, "metric"   , outMetric);
  mxSetField(plhs[0], 0, "reference", outRef);


  // Output corrected movie if so desired
  if (nlhs > 1) {
    const size_t              dimensions[]    = {size_t(imgShifted[0].rows), size_t(imgShifted[0].cols), size_t(imgShifted.size())};
    plhs[1]                   = mxCreateNumericArray(3, dimensions, mxSINGLE_CLASS, mxREAL);
    void*                     outPtr          = mxGetData(plhs[1]);
    cvMatlabCall<MatToMatlab>(imgShifted, mxGetClassID(plhs[1]), outPtr);
  }


  //---------------------------------------------------------------------------
  // Memory cleanup
  if (inputPath)
    mxFree(inputPath);
}

