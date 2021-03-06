/**
 * This file is part of S-PTAM.
 *
 * Copyright (C) 2015 Taihú Pire and Thomas Fischer
 * For more information see <https://github.com/lrse/sptam>
 *
 * S-PTAM is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * S-PTAM is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with S-PTAM. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:  Taihú Pire <tpire at dc dot uba dot ar>
 *           Thomas Fischer <tfischer at dc dot uba dot ar>
 *
 * Laboratory of Robotics and Embedded Systems
 * Department of Computer Science
 * Faculty of Exact and Natural Sciences
 * University of Buenos Aires
 */

#include "../../configuration_parser.hpp"
#include "../../../sptam/utils/macros.hpp"
#include "../../../sptam/utils/Time.hpp"
#include "../../../sptam/utils/ProgramOptions.hpp"
#include "../../FrameGenerator/IFrameGenerator.h"
#include "../../FrameGenerator/CameraFrameGenerator.h"
#include "../../FrameGenerator/VideoFileFrameGenerator.h"
#include "../../FrameGenerator/FileSequenceFrameGenerator.h"
#include "../../FrameGenerator/ListOfFilesFrameGenerator.h"

#include "../../KITTIGroundTruth.hpp"
#include "../../../sptam/MotionModel.hpp"
#include "../../../sptam/FeatureExtractorThread.hpp"

#include <signal.h>

#include <opencv2/highgui/highgui.hpp>

#ifdef SHOW_TRACKED_FRAMES

  #include "../../../sptam/utils/Draw.hpp"

  #include <X11/Xlib.h> // XInitThreads()

#endif // SHOW_TRACKED_FRAMES

// Used to smoothly finish the process in case of an interruption
bool programMustEnd = false;
// handler function called by interruptions signals
void interrupt_signal(int s) { programMustEnd = true; }

int main(int argc, char* argv[])
{


  std::string leftImages, rightImages, parametersFileYML;

  /** Define the program options */

  ProgramOptions program_options( argv[0] );
  program_options.addPositionalArgument("configuration", "configuration file with SPTAM parameters.", parametersFileYML);
  program_options.addPositionalArgument("left-images", "left camera source (image directory, streaming device or image list file).", leftImages);
  program_options.addPositionalArgument("right-images", "right camera source (image directory, streaming device or image list file).", rightImages);

  /** Parse the program options */

  try
  {
    // may throw
    program_options.parse( argc, argv );

    // if count 'help' show help and exit
    if (program_options.count("help") ) {
      std::cerr << program_options << std::endl;
      return 0;
    }

    // throws on error, so do after help in case there are any problems.
    program_options.notify();
  }
  catch(boost::program_options::error& e)
  {
    std::cerr << "ERROR: " << e.what() << std::endl << std::endl;
    std::cerr << program_options << std::endl;
    return EXIT_FAILURE;
  }

  YAML::Node config = YAML::LoadFile( parametersFileYML );


  cv::Ptr<cv::FeatureDetector> featureDetector;
  cv::Ptr<cv::DescriptorExtractor> descriptorExtractorLeft, descriptorExtractorRight;
  cv::Ptr<cv::DescriptorMatcher> descriptorMatcher;

  featureDetector = loadFeatureDetector( config["FeatureDetector"] );

  // ATENCION para FREAK necesito dos instancias del extractor!!!!
  descriptorExtractorLeft = loadDescriptorExtractor( config["DescriptorExtractor"] );
  descriptorExtractorRight = loadDescriptorExtractor( config["DescriptorExtractor"] );

  descriptorMatcher = loadDescriptorMatcher( config["DescriptorMatcher"] );

  double matchingDistanceThreshold, epipolarDistanceThreshold;

  matchingDistanceThreshold = config["MatchingDistance"].as<double>();
  epipolarDistanceThreshold = config["EpipolarDistance"].as<size_t>();


  RowMatcher rowMatcher(matchingDistanceThreshold, descriptorMatcher, epipolarDistanceThreshold);


  const size_t imageBeginIndex = 0;
  const size_t imageEndIndex = std::numeric_limits<int>::max(); // std::numeric_limits<int>::max() the whole sequence;

  IFrameGenerator* frameGeneratorLeft = NULL;
  IFrameGenerator* frameGeneratorRight = NULL;
  std::string filePrefixLeft = "";
  std::string filePrefixRight = "";
  std::string fileExtension = "png";
  frameGeneratorLeft = new FileSequenceFrameGenerator(leftImages, filePrefixLeft, fileExtension, imageBeginIndex, imageEndIndex);
  frameGeneratorRight = new FileSequenceFrameGenerator(rightImages, filePrefixRight, fileExtension, imageBeginIndex, imageEndIndex);


  // Get First Frames for use in the loop
  cv::Mat imageLeft, imageRight;
  bool hasNextFrame = frameGeneratorLeft->getNextFrame( imageLeft ) and frameGeneratorRight->getNextFrame( imageRight );

  // Main loop
  while( hasNextFrame and not programMustEnd )
  {


//  cv::Mat imageLeft, imageRight;

//  imageLeft = cv::imread( image_name_left );
//  imageRight = cv::imread( image_name_right );

  FeatureExtractorThread featureExtractorThreadLeft(imageLeft, *featureDetector, *descriptorExtractorLeft);
  FeatureExtractorThread featureExtractorThreadRight(imageRight, *featureDetector, *descriptorExtractorRight);

  featureExtractorThreadLeft.WaitUntilFinished();
  const std::vector<cv::KeyPoint>& keyPointsLeft = featureExtractorThreadLeft.GetKeyPoints();
  const cv::Mat& descriptorsLeft = featureExtractorThreadLeft.GetDescriptors();

  featureExtractorThreadRight.WaitUntilFinished();
  const std::vector<cv::KeyPoint>& keyPointsRight = featureExtractorThreadRight.GetKeyPoints();
  const cv::Mat& descriptorsRight = featureExtractorThreadRight.GetDescriptors();


  // per-row matcher for stereo rectify images
  std::vector<cv::DMatch> cvMatches;
  rowMatcher.match(keyPointsLeft, descriptorsLeft, keyPointsRight, descriptorsRight, cvMatches);

  // Return if no matches were found
  if ( cvMatches.empty() ) {
    std::cerr << "No matches found for triangulation" << std::endl;
    return -1;
  }

  std::cout << "left: " << keyPointsLeft.size() << std::endl;
  std::cout << "Right: " << keyPointsRight.size() << std::endl;
  std::cout << "matches: " << cvMatches.size() << std::endl;

  // Show Matches
  cv::Mat imageMatches, imageKeypointsLeft, imageKeypointsRight;
  cv::drawMatches(imageLeft,keyPointsLeft,imageRight, keyPointsRight,cvMatches,imageMatches);
  cv::drawKeypoints(imageLeft,keyPointsLeft,imageKeypointsLeft);
  cv::drawKeypoints(imageRight,keyPointsRight,imageKeypointsRight);

  cv::imshow("Stereo Matches", imageMatches);
  cv::imshow("Keypoints Left", imageKeypointsLeft);
  cv::imshow("Keypoints Right", imageKeypointsRight);
  cv::waitKey(0);

  // Get Next Frames
  hasNextFrame = frameGeneratorLeft->getNextFrame( imageLeft ) and frameGeneratorRight->getNextFrame( imageRight );

  }

}
