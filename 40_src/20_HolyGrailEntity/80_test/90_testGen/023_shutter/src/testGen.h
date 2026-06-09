#include "common.h"
#include <functional>
#include "cameraController.h"

namespace tstgn
{
	// メニュー表示の汎用機能
	typedef std::function<int(int, void*)> T_FUNC;
	class menuItem
	{
	public:
		std::string item;
		T_FUNC funcA;	// 'a'で実行
		T_FUNC funcB;	// 'b'の選択肢で実行
		void* any;		// any データ
	
	public:
		menuItem(const std::string& item, void* any, T_FUNC funcA, T_FUNC funcB = nullptr)
		{
			this->item = item;
			this->any = any;
			this->funcA = funcA;
			this->funcB = funcB;
		}

	};

	int menu(std::vector<tstgn::menuItem>& items);

};