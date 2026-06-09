#ifndef _CAPTURE_RUNNER_H_
#define _CAPTURE_RUNNER_H_

#include "common.h"
#include "cameraController.h"
#include "device.h"

class captureRunner
{
public:
	// 
	static errCode ready(std::vector<cmdt::fullcrum>& schedule, device& device);
	static errCode start(void);
	static errCode end(void);


protected:
};

#endif // _CAPTURE_RUNNER_H_
