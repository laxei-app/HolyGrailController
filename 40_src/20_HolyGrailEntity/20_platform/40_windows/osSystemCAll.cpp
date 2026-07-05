// osSystemCall.cpp
// windows �� OS �ˑ������������ɏW�񂷂�

#include "osSystemCall.h"
#include <thread>
#include <mutex>

namespace ossc
{
	// �X���b�h���N������B
	// return : �X���b�h�ɒʒm���邽�߂̗Ⴆ�΃^�X�NID�Ƃ���Ԃ�
	void* threadNet(THREAD_FUNC& func, void* parm, uint32_t /*stackBytes*/)
	{
		auto thread = new std::thread(func, parm);
		return thread;
	}

	// �X���b�h�̏I����҂��Ă���X���b�h��j������
	void  threadEnd(void* handle)
	{
		if (!handle) { return; }

		auto threadPtr = (std::thread*)handle;
		if (threadPtr->joinable())
		{
			threadPtr->join();
		}
		delete threadPtr;
	}

	

}
