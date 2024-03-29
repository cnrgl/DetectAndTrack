#include <iostream>
#include <fstream>
#include <sstream>

#include <chrono>
#include <cmath>
#include <future>
#include <iostream>
#include <thread>
#include <math.h>   

#include <opencv2/imgproc.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/opencv.hpp> // opencv 4.4.0 is the only completely verified version for MOSSE !!!
#include <opencv2/tracking.hpp> // contrib must be installed while runnning the opencv workspace!!!
#include <opencv2/core/ocl.hpp>
#include <opencv2/gapi/core.hpp> // GPU API library

using namespace cv;
using namespace std;

#define val 4
#define frame_ratio 15 // ratio of internal to external boxes
#define min_box_size 64

#define PI 3.14159265

#include "model.hpp"
#include "track_utils.hpp"

static float scale_h, scale_w; //scaling for convenient box size in tracking
const float ext_size = 5; // extra required size 

const char* winname = "Takip ekrani";
int mode = 0; // player modes --> play - 1 : stop - 0   || control keys:  esc --> exit , p --> pause , r--> return  
int win_size_h = 608, win_size_w = 608; // predefined fixed window sizes

std::string keys = // string keys remain default as in the sample code of YOLO, it can be changed with usage area
"{ help  h     | | Print help message. }"
"{ @alias      | | An alias name of the model to extract preprocessing parameters from models.yml file. }"
"{ zoo         | models.yml | An optional path to file with preprocessing parameters }"
"{ device      |  0 | camera device number. }"
"{ input i     | | Path to input image or video file. Skip this argument to capture frames from a camera. }"
"{ framework f | | Optional name of an origin framework of the model. Detect it automatically if it does not set. }"
"{ classes     | | Optional path to a text file with names of classes to label detected objects. }"
"{ thr         | .5 | Confidence threshold. }"
"{ nms         | .4 | Non-maximum suppression threshold. }"
"{ backend     |  0 | Choose one of the computation backends: "
"0: automatically (by default), "
"1: Halide language (http://halide-lang.org/), "
"2: Intel's Deep Learning Inference Engine (https://software.intel.com/openvino-toolkit), "
"3: OpenCV implementation, "
"4: VKCOM, "
"5: CUDA }"
"{ target      | 0 | Choose one of target computation devices: "
"0: CPU target (by default), "
"1: OpenCL, "
"2: OpenCL fp16 (half-float precision), "
"3: VPU, "
"4: Vulkan, "
"6: CUDA, "
"7: CUDA fp16 (half-float preprocess) }"
"{ async       | 0 | Number of asynchronous forwards at the same time. "
"Choose 0 for synchronous mode }";

int main(int argc, char** argv)
{
	CommandLineParser parser(argc, argv, keys);

	const std::string modelName = parser.get<String>("@alias");
	const std::string zooFile = parser.get<String>("zoo");
	keys += genPreprocArguments(modelName, zooFile);

	parser = CommandLineParser(argc, argv, keys);

	CV_Assert(parser.has("model"));
	std::string modelPath = findFile(parser.get<String>("model"));
	std::string configPath = findFile(parser.get<String>("config"));

	// model object definitons
	model_param param = { modelName, modelPath, configPath, parser.get<String>("framework"), parser.get<int>("backend"),
						parser.get<int>("target"), parser.get<int>("async") };
	model yolov4(param);
	yolov4.confThreshold = parser.get<float>("thr");
	yolov4.nmsThreshold = parser.get<float>("nms");
	yolov4.scale = parser.get<float>("scale");
	yolov4.swapRB = parser.get<bool>("rgb");
	yolov4.mean = parser.get<float>("mean");
	win_size_h = parser.get<int>("height");
	win_size_w = parser.get<int>("width");
	yolov4.inpHeigth = win_size_h;
	yolov4.inpWidth = win_size_w;

	if (parser.has("classes"))
		yolov4.get_classes(parser.get<string>("classes"));

	string filename;
	if (parser.has("input"))
		filename = parser.get<String>("input");

	Ptr<Tracker>tracker = TrackerMOSSE::create();//Tracker declaration
	scaleBox<Rect2d> scbox;

	VideoCapture video;
	if (!filename.empty()) // if video file is provided
	{
		video.open(filename);
		video.set(CAP_PROP_FRAME_WIDTH, win_size_w); // resize the screen
		video.set(CAP_PROP_FRAME_HEIGHT, win_size_h);
		cout << "file founded!!!" << endl;
	}
	else // in the absence of a video file, video streaming from a hardware cam is chosen as an input
		{
		cout<<"video test started..."<<endl; 
			video = VideoCapture("udpsrc port=5600 ! application/x-rtp, payload=26,clock-rate=90000, encoding-name=H264 ! rtpjitterbuffer mode=1 ! rtph264depay ! h264parse ! decodebin ! videoconvert ! appsink", CAP_GSTREAMER);
		}
	// Exit if the video is not opened
	if (!video.isOpened())
	{
		cout << "Could not read video file: " << video.getBackendName() << endl;
		waitKey(10);
		return 1;
	}
	
	cout << cv::getBuildInformation << endl; // get build inf - contrib is installed ?

	Mat frame, t_frame; // frame storages
	Rect2d bbox, exp_bbox; // selected bbox ROI / resized bbox
	bool track_or_detect = false;

	video.read(frame);
	while (true)
	{
		if (mode)
		{
			double timer = (double)getTickCount(); // start FPS timer
			if (!video.read(frame)) // frame read control
				break; // if frame error occurs
			
			resize(frame, frame, Size(win_size_w, win_size_h), 0.0, 0.0, INTER_CUBIC); // adjust the frame size	
			
			t_frame = frame.clone();

			
			if (!track_or_detect) // detection mode
			{
			
				// get bbox from model...
				#ifdef bypassmodel
					bbox = selectROI(t_frame);
					track_or_detect=true;
					continue;
				#endif
				float confidence = yolov4.getObject<Rect2d>(frame, bbox);
				CV_Assert(confidence > 0);
				cout << "model initiated..." << endl;

 
				exp_bbox = bbox; // stored original box in printable exp_bbox
				scale_h = min_box_size / bbox.height; // calculated scale to adjust frame according to predefined size 
				scale_w = min_box_size / bbox.width;

				cout << "scale = " << scale_h << "frame_size" << win_size_h << endl;
				win_size_h *= scale_h;
				win_size_w *= scale_w;

				resize(t_frame, t_frame, Size(win_size_w, win_size_h), 0.0, 0.0, INTER_CUBIC);
				bbox = Rect(bbox.x * scale_w - ext_size, bbox.y * scale_h - ext_size, bbox.width * scale_w + 2 * ext_size, bbox.height * scale_h + 2 * ext_size);

				//scbox.init(t_frame, bbox);
				tracker->init(t_frame, bbox); // initialize tracker
				
				destroyWindow("detections");
				track_or_detect = true; // interchange to tracking mode ...
			}
			else // tracking 
			{
				//rectangle(t_frame, bbox, Scalar(0, 250, 0));
				//imshow("resized frame", grayFrame);
				if (tracker->update(t_frame, bbox)) // tracking check
				{
					float fps = getTickFrequency() / ((double)getTickCount() - timer); // take counter

					bbox = Rect((bbox.x + ext_size) / scale_w, (bbox.y + ext_size) / scale_h, (bbox.width - 2 * ext_size) / scale_w, (bbox.height - 2 * ext_size) / scale_h);
					exp_bbox = bbox;
					//scbox.updateSize(t_frame, bbox);

					int center_x = win_size_w / scale_w / 2;
					int center_y = win_size_h / scale_h / 2;

					resize(frame, frame, Size(win_size_w / scale_w, win_size_h / scale_h), 0.0, 0.0, INTER_CUBIC);


					drawMarker(frame, Center(bbox), Scalar(0, 255, 255)); //mark the center 
					//drawMarker(frame, Point(center_x, center_y), Scalar(0,0, 255)); //mark the screen center

					putText(frame, "FPS : " + SSTR(int(fps)), Point(100, 50), FONT_HERSHEY_SIMPLEX, 0.75, Scalar(50, 170, 50), 2);

				}
				else
				{
					// Tracking failure detected.
					resize(frame, frame, Size(win_size_w / scale_w, win_size_h / scale_h), 0.0, 0.0, INTER_CUBIC);
					putText(frame, "Tracking lost...", Point(100, 80), FONT_HERSHEY_SIMPLEX, 0.75, Scalar(0, 50, 200), 2);
					win_size_h = parser.get<int>("height");
					win_size_w = parser.get<int>("width");
					tracker->clear();
					tracker = TrackerMOSSE::create();
					track_or_detect = false; // return to the detection mode ...
				}
			}
		}

		rectangle(frame, exp_bbox, Scalar(255, 0, 0), 2, 1);
		imshow(winname, frame);// show final result ...
		moveWindow(winname, 50, 50);
		//waitKey(0); // to move frame by frame -- REMOVE BEFORE FLIGHT !!!

		int keyboard = waitKey(5); // take control key from user 
		if (keyboard == 'q' || keyboard == 27) // quit
			break;
		else if (keyboard == 'p' || keyboard == 112) // pause
			mode = 0;
		else if (keyboard == 'r' || keyboard == 114) // return
			mode = 1;
	}

	// cleaning procedure in case of exit command failure

	return 0;
	}
