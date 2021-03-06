/*
 * YoloObjectDetector.cpp
 *
 *  Created on: Dec 19, 2016
 *      Author: Marko Bjelonic
 *   Institute: ETH Zurich, Robotic Systems Lab
 *
 *
 *  Modified on: May 20, 2018
 *      Authors: Alejandro Díaz, Adrian Romero and Gonzalo Nuño
 *    Institute: UPM, Universidad Politécnica de Madrid
 *
 *  Modified by: Abdur Rosyid and Ardiansyah Al Farouq
 *           in: January 2020
 */

// YOLO object detector
#include "darknet_ros/YoloObjectDetector.hpp"
#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>   /* File Control Definitions           */
#include <termios.h> /* POSIX Terminal Control Definitions */
#include <unistd.h>  /* UNIX Standard Definitions 	   */
#include <errno.h>   /* ERROR Number Definitions           */

#include <string.h>
#include <dirent.h>

#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/time.h>

#include <linux/input.h>

#include <signal.h>

//#include "FindObjectROS.h"
#include <std_msgs/Float32MultiArray.h>
#include <find_object_2d/ObjectsStamped.h>
//#include <find_object_2d/DetectionInfo.h>



#include <cmath>

// Check for xServer
#include <X11/Xlib.h>

#ifdef DARKNET_FILE_PATH
std::string darknetFilePath_ = DARKNET_FILE_PATH;
#else
#error Path of darknet repository is not defined in CMakeLists.txt.
#endif


#define xData 	8001
#define yData 	8002
#define zData 	8003
#define qData 	8004

struct daKom{
	char header[7];
	float X;
	float Y;
	float Z;
	float Q;
};

static int *resultX, *resultY, *resultZ, *resultQ;

void indivData(){
	int KeyX = xData;	int KeyY = yData;
	int KeyZ = zData;	int KeyQ = qData;

	int spaceIdX = shmget(KeyX, sizeof(int), IPC_CREAT | S_IRUSR | S_IWUSR);
	resultX = (int*)shmat(spaceIdX, NULL, 0);

	int spaceIdY = shmget(KeyY, sizeof(int), IPC_CREAT | S_IRUSR | S_IWUSR);
	resultY = (int*)shmat(spaceIdY, NULL, 0);

	int spaceIdZ = shmget(KeyZ, sizeof(int), IPC_CREAT | S_IRUSR | S_IWUSR);
	resultZ = (int*)shmat(spaceIdZ, NULL, 0);

	int spaceIdQ = shmget(KeyQ, sizeof(int), IPC_CREAT | S_IRUSR | S_IWUSR);
	resultQ = (int*)shmat(spaceIdQ, NULL, 0);

//	*resultO = datO;
	*resultX = 0;
	*resultY = 0;
	*resultZ = 0;
	*resultQ = 0;
}

namespace darknet_ros 
{
   char *cfg;
   char *weights;
   char *data;
   char **detectionNames;

   YoloObjectDetector::YoloObjectDetector(ros::NodeHandle nh)
       : nodeHandle_(nh),
         imageTransport_(nodeHandle_),
         numClasses_(0),
         classLabels_(0),
         rosBoxes_(0),
         rosBoxCounter_(0),
         imagergb_sub(imageTransport_,"/camera/color/image_raw",1),       //For depth inclussion
         imagedepth_sub(imageTransport_,"/camera/depth/image_rect_raw",1),   //For depth inclussion
         sync_1(MySyncPolicy_1(5), imagergb_sub, imagedepth_sub)        //For depth inclussion

   {
      ROS_INFO("[YoloObjectDetector] Node started.");

      // Read parameters from config file.
      if (!readParameters())
      {
         ros::requestShutdown();
      }
      init();
   }

   YoloObjectDetector::~YoloObjectDetector()
   {
      {
         boost::unique_lock<boost::shared_mutex> lockNodeStatus(mutexNodeStatus_);
         isNodeRunning_ = false;
      }
   yoloThread_.join();
   }

   bool YoloObjectDetector::readParameters()
   {
      // Load common parameters.
      nodeHandle_.param("image_view/enable_opencv", viewImage_, true);
      nodeHandle_.param("image_view/wait_key_delay", waitKeyDelay_, 3);
      nodeHandle_.param("image_view/enable_console_output", enableConsoleOutput_, false);

      // Check if Xserver is running on Linux.
      if (XOpenDisplay(NULL))
      {
         // Do nothing!
         ROS_INFO("[YoloObjectDetector] Xserver is running.");
      }
      else
      {
         ROS_INFO("[YoloObjectDetector] Xserver is not running.");
         viewImage_ = false;
      }

      // Set vector sizes.
      nodeHandle_.param("yolo_model/detection_classes/names", classLabels_, std::vector<std::string>(0));
      numClasses_ = classLabels_.size();
      rosBoxes_ = std::vector<std::vector<RosBox_> >(numClasses_);
      rosBoxCounter_ = std::vector<int>(numClasses_);

      return true;
   }

   void YoloObjectDetector::init()
   {
      ROS_INFO("[YoloObjectDetector] init().");

      // Initialize deep network of darknet.
      std::string weightsPath;
      std::string configPath;
      std::string dataPath;
      std::string configModel;
      std::string weightsModel;

      // Threshold of object detection.
      float thresh;
      nodeHandle_.param("yolo_model/threshold/value", thresh, (float) 0.3);

      // Path to weights file.
      nodeHandle_.param("yolo_model/weight_file/name", weightsModel, std::string("yolov2-tiny.weights"));
      nodeHandle_.param("weights_path", weightsPath, std::string("/default"));
      weightsPath += "/" + weightsModel;
      weights = new char[weightsPath.length() + 1];
      strcpy(weights, weightsPath.c_str());

      // Path to config file.
      nodeHandle_.param("yolo_model/config_file/name", configModel, std::string("yolov2-tiny.cfg"));
      nodeHandle_.param("config_path", configPath, std::string("/default"));
      configPath += "/" + configModel;
      cfg = new char[configPath.length() + 1];
      strcpy(cfg, configPath.c_str());

      // Path to data folder.
      dataPath = darknetFilePath_;
      dataPath += "/data";
      data = new char[dataPath.length() + 1];
      strcpy(data, dataPath.c_str());

      // Get classes.
      detectionNames = (char**) realloc((void*) detectionNames, (numClasses_ + 1) * sizeof(char*));
      for (int i = 0; i < numClasses_; i++)
      {
          detectionNames[i] = new char[classLabels_[i].length() + 1];
          strcpy(detectionNames[i], classLabels_[i].c_str());
      }

      // Load network.
      setupNetwork(cfg, weights, data, thresh, detectionNames, numClasses_, 0, 0, 1, 0.5, 0, 0, 0, 0);
      yoloThread_ = std::thread(&YoloObjectDetector::yolo, this);

      // Initialize publisher and subscriber.
      std::string cameraTopicName;
      int cameraQueueSize;
      std::string objectDetectorTopicName;
      int objectDetectorQueueSize;
      bool objectDetectorLatch;
      std::string boundingBoxesTopicName;
      int boundingBoxesQueueSize;
      bool boundingBoxesLatch;
      std::string detectionImageTopicName;
      int detectionImageQueueSize;
      bool detectionImageLatch;
      std::string objectPositionTopicName;
      int objectPositionQueueSize;
      bool objectPositionLatch;

      std::string depthTopicName;        //For depth inclussion
      int depthQueueSize;                //For depth inclussion

      nodeHandle_.param("subscribers/camera_reading/topic", cameraTopicName, std::string("/camera/image_raw"));
      nodeHandle_.param("subscribers/camera_reading/queue_size", cameraQueueSize, 1);

      nodeHandle_.param("subscribers/camera_depth/topic", depthTopicName, std::string("/depth/image_raw"));   //For depth inclussion
      nodeHandle_.param("subscribers/camera_depth/queue_size", depthQueueSize, 1);                            //For depth inclussion

      nodeHandle_.param("publishers/object_detector/topic", objectDetectorTopicName, std::string("found_object"));
      nodeHandle_.param("publishers/object_detector/queue_size", objectDetectorQueueSize, 1);
      nodeHandle_.param("publishers/object_detector/latch", objectDetectorLatch, false);

      nodeHandle_.param("publishers/bounding_boxes/topic", boundingBoxesTopicName, std::string("bounding_boxes"));
      nodeHandle_.param("publishers/bounding_boxes/queue_size", boundingBoxesQueueSize, 1);
      nodeHandle_.param("publishers/bounding_boxes/latch", boundingBoxesLatch, false);

      nodeHandle_.param("publishers/detection_image/topic", detectionImageTopicName, std::string("detection_image"));
      nodeHandle_.param("publishers/detection_image/queue_size", detectionImageQueueSize, 1);
      nodeHandle_.param("publishers/detection_image/latch", detectionImageLatch, true);

      sync_1.registerCallback(boost::bind(&YoloObjectDetector::cameraCallback,this,_1,_2));   //For depth inclussion

      nodeHandle_.param("publishers/object_position/topic", objectPositionTopicName, std::string("object_position"));
      nodeHandle_.param("publishers/object_position/queue_size", objectPositionQueueSize, 1);
      nodeHandle_.param("publishers/object_position/latch", objectPositionLatch, false);

      objectPublisher_ = nodeHandle_.advertise<std_msgs::Int8>(objectDetectorTopicName, objectDetectorQueueSize, objectDetectorLatch);
      boundingBoxesPublisher_ = nodeHandle_.advertise<darknet_ros_msgs::BoundingBoxes>(boundingBoxesTopicName, boundingBoxesQueueSize, boundingBoxesLatch);
      detectionImagePublisher_ = nodeHandle_.advertise<sensor_msgs::Image>(detectionImageTopicName, detectionImageQueueSize, detectionImageLatch);
      objectPositionPublisher_ = nodeHandle_.advertise<darknet_ros_msgs::Object>(objectPositionTopicName, objectPositionQueueSize, objectPositionLatch);

      // Action servers.
      std::string checkForObjectsActionName;
      nodeHandle_.param("actions/camera_reading/topic", checkForObjectsActionName, std::string("check_for_objects"));
      checkForObjectsActionServer_.reset(new CheckForObjectsActionServer(nodeHandle_, checkForObjectsActionName, false));
      checkForObjectsActionServer_->registerGoalCallback(boost::bind(&YoloObjectDetector::checkForObjectsActionGoalCB, this));
      checkForObjectsActionServer_->registerPreemptCallback(boost::bind(&YoloObjectDetector::checkForObjectsActionPreemptCB, this));
      checkForObjectsActionServer_->start();
   }

   void YoloObjectDetector::cameraCallback(const sensor_msgs::ImageConstPtr& msg, const sensor_msgs::ImageConstPtr& msgdepth)
   {

      ROS_DEBUG("[YoloObjectDetector] USB image received.");
      cv_bridge::CvImagePtr cam_image;
      cv_bridge::CvImageConstPtr cam_depth;

      //if (msgdepth->encoding == sensor_msgs::image_encodings::TYPE_32FC1)
      //   ROS_INFO("32FC1");
      //else if (msgdepth->encoding == sensor_msgs::image_encodings::TYPE_16UC1)
      //   ROS_INFO("16UC1");

      try
      {
         cam_image = cv_bridge::toCvCopy(msg, sensor_msgs::image_encodings::BGR8);
         cam_depth = cv_bridge::toCvCopy(msgdepth, sensor_msgs::image_encodings::TYPE_32FC1);
		 //cam_depth = cv_bridge::toCvCopy(msgdepth, sensor_msgs::image_encodings::MONO8);

         imageHeader_ = msg->header;
      }

      catch (cv_bridge::Exception& e)
      {
         ROS_ERROR("cv_bridge exception: %s", e.what());
         return;
      }
    

      if (cam_image)
      {
         {
            boost::unique_lock<boost::shared_mutex> lockImageCallback(mutexImageCallback_);
            camImageCopy_ = cam_image->image.clone();
         }
         {
            boost::unique_lock<boost::shared_mutex> lockImageStatus(mutexImageStatus_);
            imageStatus_ = true;
         }
         frameWidth_ = cam_image->image.size().width;
         frameHeight_ = cam_image->image.size().height;
      }

      if (cam_depth)
      {
         DepthImageCopy_ = cam_depth->image.clone();
	//cv::imshow("DepthImageCopy_",DepthImageCopy_);
      }

      return;
   }

   void YoloObjectDetector::checkForObjectsActionGoalCB()
   {
      ROS_DEBUG("[YoloObjectDetector] Start check for objects action.");

      boost::shared_ptr<const darknet_ros_msgs::CheckForObjectsGoal> imageActionPtr = checkForObjectsActionServer_->acceptNewGoal();
      sensor_msgs::Image imageAction = imageActionPtr->image;

      cv_bridge::CvImagePtr cam_image;

      try
      {
         cam_image = cv_bridge::toCvCopy(imageAction, sensor_msgs::image_encodings::BGR8);
      }
      
      catch (cv_bridge::Exception& e)
      {
         ROS_ERROR("cv_bridge exception: %s", e.what());
         return;
      }

      if (cam_image)
      {
         {
            boost::unique_lock<boost::shared_mutex> lockImageCallback(mutexImageCallback_);
            camImageCopy_ = cam_image->image.clone();
         }
         {
            boost::unique_lock<boost::shared_mutex> lockImageCallback(mutexActionStatus_);
            actionId_ = imageActionPtr->id;
         }
         {
            boost::unique_lock<boost::shared_mutex> lockImageStatus(mutexImageStatus_);
            imageStatus_ = true;
         }
         frameWidth_ = cam_image->image.size().width;
         frameHeight_ = cam_image->image.size().height;
      }
      return;
   }

   void YoloObjectDetector::checkForObjectsActionPreemptCB()
   {
      ROS_DEBUG("[YoloObjectDetector] Preempt check for objects action.");
      checkForObjectsActionServer_->setPreempted();
   }

   bool YoloObjectDetector::isCheckingForObjects() const
   {
      return (ros::ok() && checkForObjectsActionServer_->isActive() && !checkForObjectsActionServer_->isPreemptRequested());
   }

   bool YoloObjectDetector::publishDetectionImage(const cv::Mat& detectionImage)
   {
      if (detectionImagePublisher_.getNumSubscribers() < 1)
         return false;

      cv_bridge::CvImage cvImage;
      cvImage.header.stamp = ros::Time::now();
      cvImage.header.frame_id = "detection_image";
      cvImage.encoding = sensor_msgs::image_encodings::BGR8;
      cvImage.image = detectionImage;
      detectionImagePublisher_.publish(*cvImage.toImageMsg());

      ROS_DEBUG("Detection image has been published.");
      return true;
   }

   //double YoloObjectDetector::getWallTime()
   //{
      //struct timeval time;
      //if (gettimeofday(&time, NULL))
      //{
         //return 0;
      //}
      //return (double) time.tv_sec + (double) time.tv_usec * .000001;
   //}

   int YoloObjectDetector::sizeNetwork(network *net)
   {
      int i;
      int count = 0;
      for(i = 0; i < net->n; ++i)
      {
         layer l = net->layers[i];
         if(l.type == YOLO || l.type == REGION || l.type == DETECTION)
         {
            count += l.outputs;
         }
      }
      return count;
   }

   void YoloObjectDetector::rememberNetwork(network *net)
   {
      int i;
      int count = 0;
      for(i = 0; i < net->n; ++i)
      {
         layer l = net->layers[i];
         if(l.type == YOLO || l.type == REGION || l.type == DETECTION)
         {
            memcpy(predictions_[demoIndex_] + count, net->layers[i].output, sizeof(float) * l.outputs);
            count += l.outputs;
         }
      }
   }

   detection *YoloObjectDetector::avgPredictions(network *net, int *nboxes)
   {
      int i, j;
      int count = 0;
      fill_cpu(demoTotal_, 0, avg_, 1);

      for(j = 0; j < demoFrame_; ++j)
      {
         axpy_cpu(demoTotal_, 1./demoFrame_, predictions_[j], 1, avg_, 1);
      }

      for(i = 0; i < net->n; ++i)
      {
         layer l = net->layers[i];
         if(l.type == YOLO || l.type == REGION || l.type == DETECTION)
         {
            memcpy(l.output, avg_ + count, sizeof(float) * l.outputs);
            count += l.outputs;
         }
      }
      detection *dets = get_network_boxes(net, buff_[0].w, buff_[0].h, demoThresh_, demoHier_, 0, 1, nboxes);
      return dets;
   }

   void *YoloObjectDetector::detectInThread()
   {
      running_ = 1;
      float nms = .4;

      layer l = net_->layers[net_->n - 1];
      float *X = buffLetter_[(buffIndex_ + 2) % 3].data;
      float *prediction = network_predict(net_, X);

      rememberNetwork(net_);
      detection *dets = 0;
      int nboxes = 0;
      dets = avgPredictions(net_, &nboxes);

      if (nms > 0) do_nms_obj(dets, nboxes, l.classes, nms);

      if (enableConsoleOutput_)
      {
         printf("\033[2J");
         printf("\033[1;1H");
         printf("\nFPS:%.1f\n",fps_);
         printf("Objects:\n\n");
      }
      image display = buff_[(buffIndex_+2) % 3];
      draw_detections(display, dets, nboxes, demoThresh_, demoNames_, demoAlphabet_, demoClasses_);

      // Extract the bounding boxes and send them to ROS
      int i, j;
      int count = 0;
      for (i = 0; i < nboxes; ++i)
      {
         float xmin = dets[i].bbox.x - dets[i].bbox.w / 2.;
         float xmax = dets[i].bbox.x + dets[i].bbox.w / 2.;
         float ymin = dets[i].bbox.y - dets[i].bbox.h / 2.;
         float ymax = dets[i].bbox.y + dets[i].bbox.h / 2.;

         if (xmin < 0)
            xmin = 0;
         if (ymin < 0)
            ymin = 0;
         if (xmax > 1)
            xmax = 1;
         if (ymax > 1)
            ymax = 1;

         // Iterate through possible boxes and collect the bounding boxes
         for (j = 0; j < demoClasses_; ++j)
         {
            if (dets[i].prob[j])
            {
               float x_center = (xmin + xmax) / 2;
               float y_center = (ymin + ymax) / 2;
               float BoundingBox_width = xmax - xmin;
               float BoundingBox_height = ymax - ymin;

               // Define bounding box - BoundingBox must be 1% size of frame (3.2x2.4 pixels)
               if (BoundingBox_width > 0.01 && BoundingBox_height > 0.01)
               {
                  roiBoxes_[count].x = x_center;
                  roiBoxes_[count].y = y_center;
                  roiBoxes_[count].w = BoundingBox_width;
                  roiBoxes_[count].h = BoundingBox_height;
                  roiBoxes_[count].Class = j;
                  roiBoxes_[count].prob = dets[i].prob[j];
                  count++;
               }
            }
         }
      }

      // Create array to store found bounding boxes
      // If no object detected, make sure that ROS knows that num = 0
      if (count == 0) 
      {
         roiBoxes_[0].num = 0;
      }
      else
      {
         roiBoxes_[0].num = count;
      }

      free_detections(dets, nboxes);
      demoIndex_ = (demoIndex_ + 1) % demoFrame_;
      running_ = 0;
      return 0;
   }

   void *YoloObjectDetector::fetchInThread()
   {
      IplImage* ROS_img = getIplImage();
      ipl_into_image(ROS_img, buff_[buffIndex_]);
      {
         boost::shared_lock<boost::shared_mutex> lock(mutexImageCallback_);
         buffId_[buffIndex_] = actionId_;
      }
      rgbgr_image(buff_[buffIndex_]);
      letterbox_image_into(buff_[buffIndex_], net_->w, net_->h, buffLetter_[buffIndex_]);
      return 0;
   }

   void *YoloObjectDetector::displayInThread(void *ptr)
   {
      show_image_cv(buff_[(buffIndex_ + 1)%3], "YOLO V3", ipl_);
      int c = cvWaitKey(waitKeyDelay_);
      if (c != -1) c = c%256;
      if (c == 27)
      {
         demoDone_ = 1;
         return 0;
      }
      else if (c == 82)
      {
         demoThresh_ += .02;
      }
      else if (c == 84)
      {
         demoThresh_ -= .02;
         if(demoThresh_ <= .02) demoThresh_ = .02;
      }
      else if (c == 83)
      {
         demoHier_ += .02;
      }
      else if (c == 81)
      {
         demoHier_ -= .02;
         if(demoHier_ <= .0) demoHier_ = .0;
      }
      return 0;
   }

   void *YoloObjectDetector::displayLoop(void *ptr)
   {
      while (1)
      {
         displayInThread(0);
      }
   }

   void *YoloObjectDetector::detectLoop(void *ptr)
   {
      while (1)
      {
         detectInThread();
      }
   }

   void YoloObjectDetector::setupNetwork(char *cfgfile, char *weightfile, char *datafile, float thresh, char **names, int classes, int delay, char *prefix, int avg_frames, float hier, int w, int h, int frames, int fullscreen)
   {
      demoPrefix_ = prefix;
      demoDelay_ = delay;
      demoFrame_ = avg_frames;
      image **alphabet = load_alphabet_with_file(datafile);
      demoNames_ = names;
      demoAlphabet_ = alphabet;
      demoClasses_ = classes;
      demoThresh_ = thresh;
      demoHier_ = hier;
      fullScreen_ = fullscreen;
      printf("YOLO V3\n");
      net_ = load_network(cfgfile, weightfile, 0);
      set_batch_network(net_, 1);
   }

   void YoloObjectDetector::yolo()
   {
      const auto wait_duration = std::chrono::milliseconds(2000);
      while (!getImageStatus())
      {
         printf("Waiting for image.\n");
         if (!isNodeRunning())
         {
            return;
         }
         std::this_thread::sleep_for(wait_duration);
      }

      std::thread detect_thread;
      std::thread fetch_thread;

      srand(2222222);

      int i;
      demoTotal_ = sizeNetwork(net_);
      predictions_ = (float **) calloc(demoFrame_, sizeof(float*));

      for (i = 0; i < demoFrame_; ++i)
      {
         predictions_[i] = (float *) calloc(demoTotal_, sizeof(float));
      }

      avg_ = (float *) calloc(demoTotal_, sizeof(float));

      layer l = net_->layers[net_->n - 1];
      roiBoxes_ = (darknet_ros::RosBox_ *) calloc(l.w * l.h * l.n, sizeof(darknet_ros::RosBox_));

      IplImage* ROS_img = getIplImage();
      buff_[0] = ipl_to_image(ROS_img);
      buff_[1] = copy_image(buff_[0]);
      buff_[2] = copy_image(buff_[0]);
      buffLetter_[0] = letterbox_image(buff_[0], net_->w, net_->h);
      buffLetter_[1] = letterbox_image(buff_[0], net_->w, net_->h);
      buffLetter_[2] = letterbox_image(buff_[0], net_->w, net_->h);
      ipl_ = cvCreateImage(cvSize(buff_[0].w, buff_[0].h), IPL_DEPTH_8U, buff_[0].c);

      int count = 0;

      if (!demoPrefix_ && viewImage_)
      {
         cvNamedWindow("YOLO V3", CV_WINDOW_NORMAL);
         if (fullScreen_)
         {
            cvSetWindowProperty("YOLO V3", CV_WND_PROP_FULLSCREEN, CV_WINDOW_FULLSCREEN);
         }
         else
         {
            cvMoveWindow("YOLO V3", 0, 0);
            cvResizeWindow("YOLO V3", 640, 480);
         }
      }

      demoTime_ = what_time_is_it_now();

      while (!demoDone_)
      {
         buffIndex_ = (buffIndex_ + 1) % 3;
         fetch_thread = std::thread(&YoloObjectDetector::fetchInThread, this);
         detect_thread = std::thread(&YoloObjectDetector::detectInThread, this);
         if (!demoPrefix_)
         {
            fps_ = 1./(what_time_is_it_now() - demoTime_);
            demoTime_ = what_time_is_it_now();
            if (viewImage_)
            {
               displayInThread(0);
            }
            publishInThread();
         }
         else
         {
            char name[256];
            sprintf(name, "%s_%08d", demoPrefix_, count);
            save_image(buff_[(buffIndex_ + 1) % 3], name);
         }
         fetch_thread.join();
         detect_thread.join();
         ++count;
         if (!isNodeRunning())
         {
            demoDone_ = true;
         }
      }
   }

   IplImage* YoloObjectDetector::getIplImage()
   {
      boost::shared_lock<boost::shared_mutex> lock(mutexImageCallback_);
      IplImage* ROS_img = new IplImage(camImageCopy_);
      return ROS_img;
   }

   bool YoloObjectDetector::getImageStatus(void)
   {
      boost::shared_lock<boost::shared_mutex> lock(mutexImageStatus_);
      return imageStatus_;
   }

   bool YoloObjectDetector::isNodeRunning(void)
   {
      boost::shared_lock<boost::shared_mutex> lock(mutexNodeStatus_);
      return isNodeRunning_;
   }

   cv::Vec3f YoloObjectDetector::getDepth(const cv::Mat & depthImage,
				   int x, int y,
				   float cx, float cy,
				   float fx, float fy)
{
	if(!(x >=0 && x<depthImage.cols && y >=0 && y<depthImage.rows))
	{
		ROS_ERROR("Point must be inside the image (x=%d, y=%d), image size=(%d,%d)",
				x, y,
				depthImage.cols, depthImage.rows);
		return cv::Vec3f(
				std::numeric_limits<float>::quiet_NaN (),
				std::numeric_limits<float>::quiet_NaN (),
				std::numeric_limits<float>::quiet_NaN ());
	}


	cv::Vec3f pt;

	// Use correct principal point from calibration
	float center_x = cx; //cameraInfo.K.at(2)
	float center_y = cy; //cameraInfo.K.at(5)

	bool isInMM = depthImage.type() == CV_16UC1; // is in mm?

	// Combine unit conversion (if necessary) with scaling by focal length for computing (X,Y)
	float unit_scaling = isInMM?0.001f:1.0f;
	float constant_x = unit_scaling / fx; //cameraInfo.K.at(0)
	float constant_y = unit_scaling / fy; //cameraInfo.K.at(4)
	float bad_point = std::numeric_limits<float>::quiet_NaN ();

	float depth;
	bool isValid;
	if(isInMM)
	{
		depth = (float)depthImage.at<uint16_t>(y,x);
		isValid = depth != 0.0f;
	}
	else
	{
		depth = depthImage.at<float>(y,x);
		isValid = std::isfinite(depth);
	}

	// Check for invalid measurements
	if (!isValid)
	{
		pt.val[0] = pt.val[1] = pt.val[2] = bad_point;
	}
	else
	{
		// Fill in XYZ
		pt.val[0] = (float(x) - center_x) * depth * constant_x;
		pt.val[1] = (float(y) - center_y) * depth * constant_y;
		pt.val[2] = depth*unit_scaling;
	}
	return pt;
}

   float YoloObjectDetector::getDepth2(const cv::Mat & depthImage, int xmin, int ymin, int xmax, int ymax){
	float Value = 0, GrayValue=0;	
	int Ind	= 0;
	for(int i=xmin; i<=xmax; i++)
	 for(int j=ymin; j<=ymax; j++)
	 {
	    Value=(float)depthImage.at<float>(j,i);
	    if (Value==Value && (float)depthImage.at<float>(j,i) != 0)
	    {
	       GrayValue+=Value; //Sumamos el nivel de gris del pixel  i-esimo
	       Ind++;
	    }
	 }

	return GrayValue=GrayValue/Ind;	
   }

   void *YoloObjectDetector::publishInThread()
   {
      // Publish image.
	static int fl = 0;
	static struct daKom* WData = (struct daKom*) malloc(sizeof(struct daKom));
	if (fl==0) {indivData(); fl=1;}
	static int tt=0;
        static tf::TransformBroadcaster br;
        static tf::Transform transform;
      cv::Mat cvImage = cv::cvarrToMat(ipl_);
      if (!publishDetectionImage(cv::Mat(cvImage)))
      {
         ROS_DEBUG("Detection image has not been broadcasted.");
      }

      // Publish bounding boxes and detection result.
      int num = roiBoxes_[0].num;
      if (num > 0 && num <= 100)
      {
         for (int i = 0; i < num; i++)
         {
            for (int j = 0; j < numClasses_; j++)
            {
               if (roiBoxes_[i].Class == j)
               {
                  rosBoxes_[j].push_back(roiBoxes_[i]);
                  rosBoxCounter_[j]++;
               }
            }
         }

         std_msgs::Int8 msg;
         msg.data = num;
         objectPublisher_.publish(msg);
	int boundingOk=0, i_, j_;
	float X_, Y_, Z_;

         for (int i = 0; i < numClasses_; i++)
         {
            if (rosBoxCounter_[i] > 0)
            {
               //darknet_ros_msgs::BoundingBox boundingBox;
               //darknet_ros_msgs::ObjectPosition objectPosition;

               //tf::TransformBroadcaster br;
               //tf::Transform transform;

               for (int j = 0; j < rosBoxCounter_[i]; j++)
               {
			boundingOk = 1;
			i_=i;
			j_=j;
			X_=X;
			Y_=Y;
			Z_=Z;
               }
            }
         }
	
	if (boundingOk==1){
               darknet_ros_msgs::BoundingBox boundingBox;
               darknet_ros_msgs::ObjectPosition objectPosition;

                  int xmin = (rosBoxes_[i_][j_].x - rosBoxes_[i_][j_].w / 2) * frameWidth_;
                  int ymin = (rosBoxes_[i_][j_].y - rosBoxes_[i_][j_].h / 2) * frameHeight_;
                  int xmax = (rosBoxes_[i_][j_].x + rosBoxes_[i_][j_].w / 2) * frameWidth_;
                  int ymax = (rosBoxes_[i_][j_].y + rosBoxes_[i_][j_].h / 2) * frameHeight_;

                  YoloObjectDetector::Coordinates(i_, xmin, ymin, xmax, ymax);

                  boundingBox.Class = classLabels_[i_];
                  boundingBox.probability = rosBoxes_[i_][j_].prob;
                  boundingBox.xmin = xmin;
                  boundingBox.ymin = ymin;
                  boundingBox.xmax = xmax;
                  boundingBox.ymax = ymax;
                  //boundingBox.X = X;
                  //boundingBox.Y = Y;
                  //boundingBox.Z = Z;
                  boundingBox.Invalid = Invalid;
                  boundingBoxesResults_.bounding_boxes.push_back(boundingBox);

                  objectPosition.X = X_;
                  objectPosition.Y = Y_;
                  objectPosition.Z = Z_;
                  objectPosition_.object_position_array.push_back(objectPosition);
			
                  //ros::Rate rate(100.0);
                  //if (nodeHandle_.ok()){
			//printf("\ntt=%d\n",tt++);
                   //transform.setOrigin( tf::Vector3(0.0, 0.0, 0.0) );
//                   transform.setOrigin( tf::Vector3(X,Y,Z) );
                   //tf::Quaternion q;
                   //q.setRPY(0, 0, msg->theta);
                   //transform.setRotation(q);
//                   transform.setRotation( tf::Quaternion(0, 0, 0, 1) );
//                   br.sendTransform(tf::StampedTransform(transform, ros::Time::now(), "camera_link", "object"));
                   //rate.sleep();
                  //}

			*resultX = X_*1000000.0;
			*resultY = Y_*1000000.0;
			*resultZ = Z_*1000000.0;
			*resultQ = 1;
			//QTransform>::const_iterator iter=info.objDetected_.constBegin();
			float cx = 327.8558654785156, cy = 247.04779052734375;
			float fx = 614.0160522460938, fy = 614.0221557617188;
			int U = xmax-xmin;
			int V = ymax-ymin;

//			QPointF xAxis;
//			QPointF yAxis;
			float center_x = xmin + (U/2);	
			float center_y = ymin + (V/2);

			float xAxis_x = xmin + (3*U/4);	
			float xAxis_y = ymin + (V/2);
			
			float yAxis_x = xmin + (U/2);	
			float yAxis_y = ymin + (3*V/4);

			printf("%lf - %lf\n", center_x ,center_y);
			printf("%lf - %lf\n", xAxis_x ,xAxis_y);
			printf("%lf - %lf\n", yAxis_x ,yAxis_y);
			printf("%d - %d\n", U ,V);


			cv::Vec3f center3D = this->getDepth(DepthImageCopy_,
					center_x, center_y,
					cx, cy,
					fx, fy);
			cv::Vec3f axisEndX = this->getDepth(DepthImageCopy_,
					xAxis_x, xAxis_y,
					cx, cy,
					fx, fy);
			cv::Vec3f axisEndY = this->getDepth(DepthImageCopy_,
					yAxis_x, yAxis_y,
					cx, cy,
					fx, fy);

/*			cv::Vec3f center3D;
			cv::Vec3f axisEndX;
			cv::Vec3f axisEndY;

//			center3D.val[2]=(float)DepthImageCopy_.at<float>(center_y,center_x);
//			axisEndX.val[2]=(float)DepthImageCopy_.at<float>(xAxis_y,xAxis_x);
//			axisEndY.val[2]=(float)DepthImageCopy_.at<float>(yAxis_y,yAxis_x);

			center3D.val[2] = this->getDepth2(DepthImageCopy_, center_x-(U/20), center_y-(V/20), center_x+(U/20), center_y+(V/20) );
			axisEndX.val[2] = this->getDepth2(DepthImageCopy_, xAxis_x-(U/20), xAxis_y-(V/20), xAxis_x+(U/20), xAxis_y+(V/20) );
			axisEndY.val[2] = this->getDepth2(DepthImageCopy_, yAxis_x-(U/20), yAxis_y-(V/20), yAxis_x+(U/20), yAxis_y+(V/20) );

//			center3D.val[2] = axisEndX.val[2] = axisEndY.val[2] = Z;

			center3D.val[0]=center3D.val[2]*(center_x-cx)/fx;
			axisEndX.val[0]=axisEndX.val[2]*(xAxis_x-cx)/fx;
			axisEndY.val[0]=axisEndY.val[2]*(yAxis_x-cx)/fx;
			
			center3D.val[1]=center3D.val[2]*(center_y-cy)/fy;
			axisEndX.val[1]=axisEndX.val[2]*(xAxis_y-cy)/fy;
			axisEndY.val[1]=axisEndY.val[2]*(yAxis_y-cy)/fy;
				
*/	
	
			tf::StampedTransform transform_;
			transform_.setIdentity();
			transform_.child_frame_id_ = "object";
			transform_.frame_id_ = "camera_link";
			transform_.stamp_ = ros::Time::now();
			
			printf("%lf - %lf - %lf\n", center3D.val[0], center3D.val[1] ,center3D.val[2]);
			printf("%lf - %lf - %lf\n", axisEndX.val[0], axisEndX.val[1] ,axisEndX.val[2]);
			printf("%lf - %lf - %lf\n", axisEndY.val[0], axisEndY.val[1] ,axisEndY.val[2]);
			
			//set rotation (y inverted)
			tf::Vector3 xAxis(axisEndX.val[0] - center3D.val[0], axisEndX.val[1] - center3D.val[1], axisEndX.val[2] - center3D.val[2]);
			xAxis.normalize();
			tf::Vector3 yAxis(axisEndY.val[0] - center3D.val[0], axisEndY.val[1] - center3D.val[1], axisEndY.val[2] - center3D.val[2]);
			yAxis.normalize();
			tf::Vector3 zAxis = xAxis*yAxis;
			tf::Matrix3x3 rotationMatrix(
						xAxis.x(), yAxis.x() ,zAxis.x(),
						xAxis.y(), yAxis.y(), zAxis.y(),
						xAxis.z(), yAxis.z(), zAxis.z());
			/*			
			printf("%lf - %lf - %lf\n", xAxis.x(), yAxis.x() ,zAxis.x());
			printf("%lf - %lf - %lf\n", xAxis.y(), yAxis.y(), zAxis.y());
			printf("%lf - %lf - %lf\n", xAxis.z(), yAxis.z(), zAxis.z());
			*/
			tf::Quaternion q;
			rotationMatrix.getRotation(q);
			transform_.setOrigin(tf::Vector3(X,Y,Z));
			transform_.setRotation( tf::Quaternion(q) );			
			transform_.setRotation(q.normalized());
			printf("q : %lf - %lf - %lf - %lf\n", q[0], q[1], q[2], q[3]);
			br.sendTransform(transform_);

			//if(Z > Zavg-100){}
			//else if(Z < Zavg+100){}	
	}

         boundingBoxesResults_.header.stamp = ros::Time::now();
         boundingBoxesResults_.header.frame_id = "detection";
         boundingBoxesResults_.image_header = imageHeader_;
         boundingBoxesPublisher_.publish(boundingBoxesResults_);

         objectPositionPublisher_.publish(objectPosition_);

      }
      
      else
      {
         std_msgs::Int8 msg;
         msg.data = 0;
         objectPublisher_.publish(msg);
      }

      if (isCheckingForObjects())
      {
         ROS_DEBUG("[YoloObjectDetector] check for objects in image.");
         darknet_ros_msgs::CheckForObjectsResult objectsActionResult;
         objectsActionResult.id = buffId_[0];
         objectsActionResult.bounding_boxes = boundingBoxesResults_;
         checkForObjectsActionServer_->setSucceeded(objectsActionResult, "Send bounding boxes.");
      }

      boundingBoxesResults_.bounding_boxes.clear();
      for (int i = 0; i < numClasses_; i++)
      {
         rosBoxes_[i].clear();
         rosBoxCounter_[i] = 0;
      }

      return 0;
   }

   void YoloObjectDetector::Coordinates(int ObjID, int xmin, int ymin, int xmax, int ymax)
   {
      //int U = ((xmax-xmin)+xmin);
      //int V = ((ymax-ymin)+ymin);
      int x = ((xmin+xmax)/2);
      int y = ((ymin+ymax)/2);
      int Ind=0;
      float GrayValue=0;
      float Value=0;
      cv::Mat Img;
      cv::Mat Binaria1;
      cv::Mat Binaria2;

      cv::cvtColor(camImageCopy_, Img, cv::COLOR_BGR2HSV);

      if (ObjID==0 || ObjID==3 || ObjID==6 || ObjID==9)  //Rojo
      {
         cv::Scalar RojosBajos1(0,65,75);
         cv::Scalar RojosAltos1(12,255,255);
         cv::Scalar RojosBajos2(240,65,75);
         cv::Scalar RojosAltos2(256,255,255);

         cv::inRange(Img, RojosBajos1, RojosAltos1, Binaria1);
         cv::inRange(Img, RojosBajos2, RojosAltos2, Binaria2);
         cv::add(Binaria1,Binaria2,Binaria1);
		   //cv::imshow("Rojo",Binaria1);
      }

      else if (ObjID==1 || ObjID==4 || ObjID==7 || ObjID==10)  //Azul
      {
         cv::Scalar AzulesBajos(100,65,75);
         cv::Scalar AzulesAltos(130,255,255);

         cv::inRange(Img, AzulesBajos, AzulesAltos, Binaria1);
         //cv::imshow("Azul",Binaria1);
      }

      else if (ObjID==2 || ObjID==5 || ObjID==8 || ObjID==11)  //Verde
      {
         cv::Scalar VerdesBajos(49,50,50);
         cv::Scalar VerdesAltos(107,255,255);

         cv::inRange(Img, VerdesBajos, VerdesAltos, Binaria1);
         //cv::imshow("Verde",Binaria1);
      }

      else if (ObjID==12)  //Amarillo
      {
         cv::Scalar AmarillosBajos(20,100,100);
         cv::Scalar AmarillosAltos(30,255,255);

         cv::inRange(Img, AmarillosBajos, AmarillosAltos, Binaria1);
         //cv::imshow("Amarillo",Binaria1);
      }

      else if (ObjID==13)  //Bomba
      {      
         cv::Scalar NegrosBajos(0,0,0);   
         cv::Scalar NegrosAltos(0,0,10);
 
         cv::inRange(Img, NegrosBajos, NegrosAltos, Binaria1);
         //cv::imshow("Negro",Binaria1);
      }
	
	  for(int i=xmin; i<=xmax; i++)
         for(int j=ymin; j<=ymax; j++)
         {
            Value=(float)DepthImageCopy_.at<float>(j,i);
            if (Value==Value && ((int)Binaria1.at<uchar>(j,i))!=0)
            {
               GrayValue+=Value; //Sumamos el nivel de gris del pixel  i-esimo
               Ind++;
            }
         }

	  GrayValue=GrayValue/Ind;

	  Invalid= true;
	  if (GrayValue!=0)
      {
         Invalid= false;

         Z=GrayValue/1000;                                                        //Depth in meter
         //X=(((U-320.5)*Z)/554.254691191187)/1000;                               //X=((U-Cx)*Z)/fx in meter
         //Y=(((V-240.5)*Z)/554.254691191187)/1000;                               //Y=((V-Cy)*Z)/fy in meter
         X=((((float(x)-327.8558654785156)*Z)/614.0160522460938)-(1000*(-0.001)))/1000;  //X=((U-Cx)*Z)/fx in meter
         Y=((((float(y)-247.04779052734375)*Z)/614.0221557617188)-(1000*0.015))/1000;    //Y=((V-Cy)*Z)/fy in meter
         //Subtraction in X and Y is based on the translation of rosrun tf tf_echo /camera_color_frame /camera_depth_frame

         ROS_INFO("X %f, Y %f, Z %f ,Invalido %d", X, Y, Z, Invalid);
	
      }
   }
}
