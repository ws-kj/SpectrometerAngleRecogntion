#include <opencv2/opencv.hpp>
#include <tesseract/baseapi.h>
#include <leptonica/allheaders.h>
#include <cstdio>

#include "spectreye.h"


Spectreye::Spectreye(int debug) 
{
	this->dkernel = cv::getStructuringElement(cv::MORPH_DILATE, cv::Size(4,4));
	this->okernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(1,1));
	this->ckernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(2,4));

	this->net = cv::dnn::readNet("../east.pb");
	this->lsd = cv::createLineSegmentDetector(0);

	this->clahe = cv::createCLAHE();
	this->clahe->setClipLimit(2.0);
	this->clahe->setTilesGridSize(cv::Size(8, 8));

	this->tess = new tesseract::TessBaseAPI();
	this->tess->Init(NULL, "eng");
	this->tess->SetPageSegMode(tesseract::PageSegMode::PSM_SPARSE_TEXT);

	this->debug = debug;
}

std::string Spectreye::GetAngleHMS(std::string path, double encoder_angle) 
{
	return this->FromFrame(cv::imread(path), DT_HMS, path, encoder_angle);
}

std::string Spectreye::GetAngleSHMS(std::string path, double encoder_angle) 
{
	return this->FromFrame(cv::imread(path), DT_SHMS, path, encoder_angle);
}

std::string Spectreye::ExtractTimestamp(std::string path) 
{
	return "not implemented";
}

cv::Mat Spectreye::CLAHEFilter(cv::Mat frame, int passes) {
	cv::Mat img, lab;
	cv::cvtColor(frame, lab, cv::COLOR_BGR2Lab);
	
	std::vector<cv::Mat> lplanes(3);
	cv::split(lab, lplanes);
	cv::Mat dst;
	for(int i=0; i<passes; i++)
		this->clahe->apply(lplanes[0], dst);
	dst.copyTo(lplanes[0]);
	cv::merge(lplanes, lab);
	cv::cvtColor(lab, img, cv::COLOR_Lab2BGR);

	return img;
}

cv::Mat Spectreye::ThreshFilter(cv::Mat frame) {
	cv::Mat img, bg;
	cv::threshold(frame, img, 127, 255, cv::THRESH_BINARY_INV);
	cv::morphologyEx(img, bg, cv::MORPH_DILATE, this->dkernel);
	cv::divide(img, bg, img, 255);
	cv::GaussianBlur(img, img, cv::Size(3, 3), 0);
	cv::morphologyEx(img, img, cv::MORPH_OPEN, this->okernel);
	cv::GaussianBlur(img, img, cv::Size(5, 5), 0);
	cv::fastNlMeansDenoising(img, img, 21, 7, 21);

	return img;
}

cv::Mat Spectreye::MaskFilter(cv::Mat frame) {
	cv::Mat mask, img;
	img = this->CLAHEFilter(frame, 1);

	cv::fastNlMeansDenoising(img, img, 21, 7, 21);
	cv::inRange(img, cv::Scalar(0, 0, 0), cv::Scalar(190, 190, 250), mask);
	cv::bitwise_or(img, img, img, mask=mask);
	cv::cvtColor(img, img, cv::COLOR_BGR2GRAY);
	cv::threshold(img, img, 1, 255, cv::THRESH_BINARY);
	cv::morphologyEx(img, img, cv::MORPH_OPEN, this->okernel);
	cv::GaussianBlur(img, img, cv::Size(5, 5), 0);
	cv::fastNlMeansDenoising(img, img, 21, 7, 21);

	return img;
}

std::vector<cv::Rect> Spectreye::OcrEast(cv::Mat frame) 
{
	std::vector<cv::Mat> outs;
	
	cv::Mat timg;
	cv::cvtColor(frame, timg, cv::COLOR_GRAY2BGR);

	int H = timg.size().height;
	int W = timg.size().width;

	int origH = H;
	int newW = 320, newH = 320;
	double rW = (double) W / (double) newW;
	double rH = (double) H / (double) newH;

	cv::resize(timg, timg, cv::Size(newW, newH));
	H = timg.size().height;
	W = timg.size().width;

	cv::Mat blob = cv::dnn::blobFromImage(
			timg, 1.0, cv::Size(H, W), cv::Scalar(123.68, 116.78, 103.94), true, false); 

	this->net.setInput(blob);
	this->net.forward(outs, this->layer_names);

	cv::Mat scores   = outs[0];
	cv::Mat geometry = outs[1];

	std::vector<cv::Rect> rects;
	std::vector<float> confidences;

	int nrows = scores.size[2];
	int ncols = scores.size[3];

	for(int y=0; y<nrows; ++y) {
		const float* scores_data = scores.ptr<float>(0, 0, y);
		const float* xdata0 = geometry.ptr<float>(0, 0, y);
		const float* xdata1 = geometry.ptr<float>(0, 1, y);
		const float* xdata2 = geometry.ptr<float>(0, 2, y);
		const float* xdata3 = geometry.ptr<float>(0, 3, y);
		const float* angles = geometry.ptr<float>(0, 4, y);

		for(int x=0; x<ncols; ++x) {
			if (scores_data[x] < 0.5) {
				continue;
			}
			float offsetX = x * 4.0;
			float offsetY = y * 4.0;
			float angle = angles[x];
			float cos = std::cos(angle);
			float sin = std::sin(angle);
			float h = xdata0[x] + xdata2[x];
			float w = xdata1[x] + xdata3[x];

			int endX = (int)(offsetX + (cos * xdata1[x]) + (sin * xdata2[x]));
			int endY = (int)(offsetY - (sin * xdata1[x]) + (cos * xdata2[x]));
			int startX = (int)(endX - w);
			int startY = (int)(endY - h);
			
			if (endY*rH < origH) {
				rects.push_back(cv::Rect(startX*rW, startY*rH, (endX-startX)*rW, (endY-startY)*rH));
				confidences.push_back(scores_data[x]);
			}
		}
	}

	std::vector<int> indices;
	cv::dnn::NMSBoxes(rects, confidences, 0, 0.5, indices);

	return rects;
}

std::vector<cv::Rect> Spectreye::OcrTess(cv::Mat frame) 
{
	cv::Mat img, lab;	

	cv::cvtColor(frame, frame, cv::COLOR_GRAY2BGR);
	cv::cvtColor(frame, lab, cv::COLOR_BGR2Lab);
	
	std::vector<cv::Mat> lplanes(3);
	cv::split(lab, lplanes);
	cv::Mat dst;
	this->clahe->apply(lplanes[0], dst);
	dst.copyTo(lplanes[0]);
	cv::merge(lplanes, lab);
	cv::cvtColor(lab, img, cv::COLOR_Lab2BGR);

	cv::fastNlMeansDenoising(img, img, 21, 7, 21);
	cv::GaussianBlur(img, img, cv::Size(3, 3), 0);
	cv::cvtColor(img, img, cv::COLOR_BGR2GRAY);
	cv::fastNlMeansDenoising(img, img, 21, 7, 21);
	cv::GaussianBlur(img, img, cv::Size(3, 3), 0);
	cv::fastNlMeansDenoising(img, img, 21, 7, 21);

	this->tess->SetImage((unsigned char*)img.data, img.size().width, img.size().height, 
			img.channels(), img.step1());
	this->tess->Recognize(0);
	tesseract::ResultIterator* ri = this->tess->GetIterator();
	tesseract::PageIteratorLevel level = tesseract::RIL_WORD;

	std::vector<cv::Rect> rects;

	if(ri != 0) {
		do {
			float conf = ri->Confidence(level);
			int x1, y1, x2, y2;
			ri->BoundingBox(level, &x1, &y1, &x2, &y2);

			if(conf > 40 && y2 <= img.size().height/2) {
				rects.push_back(cv::Rect(x1, y1, x2, y2));
			}

		} while (ri->Next(level));
	}

	return rects;
}

int Spectreye::FindTickCenter(cv::Mat img, int ytest, int xtest, int delta) 
{
	int optl = 0, optr = 0;

	for(int x=xtest-1; x>=1; x--) {
		if(img.at<unsigned char>(ytest, x) > img.at<unsigned char>(ytest, x-1)+delta) {
			optl = x;
			break;
		}
	}
	for(int x=xtest; x<img.size().width; x++) {
		if(img.at<unsigned char>(ytest, x) > img.at<unsigned char>(ytest, x+1)+delta) {
			optr = x;
			break;
		}
	}
	
	return (std::abs(xtest-optl) < std::abs(xtest-optr)) ? optl : optr;	
}

std::string Spectreye::FromFrame(
		cv::Mat frame, DeviceType dtype, std::string ipath, double enc_angle)
{

	int x_mid = frame.size().width/2;
	int y_mid = frame.size().height/2;
	std::string timestamp = this->ExtractTimestamp(ipath);

	cv::Mat img, display;
	cv::cvtColor(frame, img, cv::COLOR_BGR2GRAY);
	display = frame.clone();
	
	cv::Vec4f ltick, rtick;
	std::vector<cv::Vec4f> segments;

	// equiv of get_ticks() from spectreye.py
	cv::Mat pass1;
	cv::fastNlMeansDenoising(img, pass1, 21, 7, 21);
	cv::GaussianBlur(pass1, pass1, cv::Size(5, 5), 0);
	
	std::vector<cv::Vec4f> lines;
	this->lsd->detect(pass1, lines);
	ltick = lines[0];
	rtick = lines[1];

	for(int i=0; i<lines.size(); i++) {
		cv::Vec4f l = lines[i];

		if(std::abs(l[0] - l[2]) < std::abs(l[1] - l[3])) {
			if(l[1] > y_mid - (0.1 * img.size().height) &&
					l[3] < y_mid + (0.1 * img.size().height)) {
				segments.push_back(l);

				if(l[0] < x_mid && std::abs(x_mid - l[0]) < std::abs(x_mid - ltick[0])) 
					ltick = l;	
				if(l[0] > x_mid && std::abs(x_mid - l[0]) < std::abs(x_mid - rtick[0])) 
					rtick = l;
			}
		}
	}

	int true_mid, ysplit;
	float pixel_ratio;

	// equiv of find_mid from spectreye.py	
	cv::GaussianBlur(img, pass1, cv::Size(5, 5), 0);
	cv::fastNlMeansDenoising(pass1, pass1, 21, 7, 21);

	cv::Vec4f mid = segments[0];
	std::vector<cv::Vec4f> opts;
	for(auto l : segments) {
		if(ltick[1] >= l[1] and ltick[3] <= l[3]) {
			opts.push_back(l);
			if(l[3] - l[1] > mid[3] - mid[1]) 
				mid = l;
		}
	}

	std::vector<cv::Vec4f> cull;
	for(const auto& l : opts) {
		if(!(l[1] < ltick[1]-(ltick[3]-ltick[1]))) {
			cull.push_back(l);
		} else if(!(l[3] > ltick[3]+(ltick[3]-ltick[1]))) {
			cull.push_back(l);
		}
	}

	for(auto l : cull) {
		if(std::abs(x_mid - l[0]) < std::abs(x_mid - mid[0]))
			mid = l;
	}

	// equiv of proc_peak() from spectreye.py
	int y = mid[1] + (mid[3]-mid[1])/2;

	std::vector<int> ticks;
	for(int x=0; x<img.size().width; x++) {
		if(img.at<unsigned char>(y, x) > img.at<unsigned char>(y, x+1)+1 &&
				img.at<unsigned char>(y, x) > img.at<unsigned char>(y, x-1)+1) {
			ticks.push_back(x);
		}
	}

	std::vector<int> diffs;
	for(int i=0; i<ticks.size()-1; i++) 
		diffs.push_back(std::abs(ticks[i]-ticks[i+1]));
	
	std::unordered_map<int, int> freq_count;
	for(const auto& d : diffs) 
		freq_count[d]++;

	using ptype = decltype(freq_count)::value_type; // c++11 got me feeling funny
	auto mfreq = std::max_element(freq_count.begin(), freq_count.end(),
			[] (const ptype &x, const ptype &y) {return x.second < y.second;});
	pixel_ratio = std::abs(mfreq->first);


	std::vector<int> iheights;
	int uy, dy;
	for(const auto& l : ticks) {
		uy = y;
		while(img.at<unsigned char>(uy, l) < img.at<unsigned char>(uy-1,l)+5)
			uy--;
		dy = y;
		ysplit = uy;
		while(img.at<unsigned char>(dy, l) < img.at<unsigned char>(dy+1,l)+5)
			dy++;
		iheights.push_back(dy-uy);
	}

	std::vector<int> opti, heights, locs, dists;
	
	std::priority_queue<std::pair<int, int>> q;
	for(int i=0; i<iheights.size(); ++i) {
		q.push(std::pair<int, int>(iheights[i], i));
	}
	for(int i=0; i<5; ++i) {
		int ki = q.top().second;
		opti.push_back(ki);
		q.pop();
	}

	for(const auto& i : opti) {
		heights.push_back(iheights[i]);
		locs.push_back(ticks[i]);
	}

	for(const auto& l : locs)
		dists.push_back(std::abs(x_mid - l));

	std::vector<int> dsorted = dists;
	std::sort(dsorted.begin(), dsorted.end());

	// this stuff is probably broken
	auto iter = std::find(dists.begin(), dists.end(), dsorted[0]);
	true_mid = locs[iter - dists.begin()];
	
	cv::rectangle(display, cv::Point(true_mid, 0), cv::Point(true_mid, display.size().height), 
			cv::Scalar(255, 255, 0), 1);

	cv::Mat timg = this->ThreshFilter(img);

	std::vector<cv::Rect> boxes = this->OcrEast(timg);

	if(boxes.size() == 0) {
		timg = this->MaskFilter(timg);
		boxes = this->OcrEast(timg);
		if(boxes.size() == 0)
			boxes = this->OcrTess(timg);
	}

	if(boxes.size() == 0) {
		printf("failure\n");
		return "failure";
	}

	int cmpX, botY, bH;
	cv::Rect boxdata;
	for(const auto& rect : boxes) {
		int startX = rect.x;
		int startY = rect.y;
		int endX = rect.x + rect.width;
		int endY = rect.y + rect.height;

		int tpos = startX + (endX-startX)/2;

		if (std::abs(x_mid - tpos) < std::abs(x_mid - cmpX) && endY < ysplit) {
			cmpX = tpos;
			botY = endY;

			bH = std::abs(endY-startY)/2;

			boxdata = cv::Rect(
				std::max(0, startX-this->npad),
				std::max(0, startY-(this->npad/2)),
				std::min(img.size().width,  (endX-startX)+this->npad*2),
				std::min(img.size().height, (endY-startY)+(this->npad)+(this->npad/2))
			);
		}
	}
	cv::rectangle(display, boxdata, cv::Scalar(0, 255, 0), 2);

	cv::Vec4f tseg = segments[0];
	for(const auto& l : segments) {
		if(std::abs(cmpX - l[0]) < std::abs(cmpX-tseg[0]) && 
				l[1] > botY && l[1] < ysplit)
			tseg = l;
		
	}
	int tmidy = tseg[1] + ((tseg[3]-tseg[1])/4)*3;
	int tick = tseg[0];
	if(tick > boxdata.x+boxdata.width || tick < boxdata.x)
		tick = boxdata.x + boxdata.width/2; 

	cv::rectangle(display, cv::Point(tick, 0), cv::Point(tick, display.size().height), cv::Scalar(255, 0, 0), 1);


	int pix_frac = true_mid - tick;
	double dec_frac = ((double)pix_frac/pixel_ratio)*0.01;

	cv::Mat numbox = cv::Mat(pass1, boxdata);
	cv::cvtColor(numbox, numbox, cv::COLOR_GRAY2BGR);
	numbox = this->CLAHEFilter(numbox, 2);

	this->tess->SetImage((unsigned char*)numbox.data, numbox.size().width, numbox.size().height, 
			numbox.channels(), numbox.step1());
	std::string rawnum = this->tess->GetUTF8Text();

	std::cout << "raw: " << rawnum << std::endl;

	std::string nstr;
	for(const auto& n : rawnum) {
		if(std::isdigit(n))
			nstr += n;
		if(n == '\n')
			break;
	}
	if(nstr.length() > 3)
		nstr += ".0";
	else
		nstr.insert(2, ".");
	
	int tickR = 1;
	double mark = std::stod(nstr);
	std::cout << "mark: " << mark << std::endl;

	if(dtype == DT_SHMS) {
		tickR = -1;
		if(mark > SHMS_MAX)
			mark /= 10;
		if(mark < SHMS_MIN)
			mark = 0;
	} else if(dtype == DT_HMS) {
		if(mark > HMS_MAX)
			mark /= 10;
		if(mark < HMS_MIN)
			mark = 0;
	}

	double ns1   = mark + (tickR * dec_frac);
	double pow   = std::pow(10.0f, 2);
	double angle = std::round(ns1 * pow)/pow;
	std::cout << "calculated: " << angle << std::endl;	

	cv::imshow("numbox", numbox);
	cv::imshow("final", display);
	for(;;) {
		auto key = cv::waitKey(1);
		if(key == 113)
			break;
	}
	cv::destroyAllWindows();

	this->tess->End();

	return "";	
}

int main(int argc, char** argv) {
	Spectreye* s = new Spectreye(true);
	std::string res;

	if(argc == 1) {
		res = s->GetAngleHMS("../../images/qtest/HMS_0.jpg");
	} else {
		std::string path = argv[1];
		if(path.find("SHMS") != std::string::npos)
			res = s->GetAngleSHMS(path);
		else
			res = s->GetAngleHMS(path);
	}

	return 0;
}

