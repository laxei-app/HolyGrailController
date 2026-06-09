#include "testGen.h"

namespace tstgn
{
	// メニューの汎用機能
	int menu(std::vector<menuItem>& items)
	{
		int row, col;
		cons::getRowCol(row, col);

		int key = 'c';
		int select = 0;
		for (;;)
		{
			cons::setRowCol(row, col);
			auto fSelect = [&](int lin)->void
			{
				if (select == lin)	
                { 
                    cons::attr(col::YEL); 
                }
				else				{ cons::attr(col::WHT); }

				cons::printf("%s\n", items[lin].item.c_str());
				cons::attr(col::WHT);
			};

			// メニュー表示
			for (int ix = 0; ix < items.size(); ix++) { fSelect(ix); }

            // 選択肢表示
            cons::printf("a:action, b:select, c:exit\n");

            // 選択時動作は先にやる
            if (items[select].funcB != nullptr)
            {   // 選択時機能実行
                (items[select].funcB)(select, items[select].any);
            }

            // 動作
            key = cons::getch();
            if (key == 'b') 
            {	// 項目の選択
                select = (select + 1) % items.size(); 
                continue;
            }
            // メニューを消す
            cons::setRowCol(row, col);
            for (int ix = 0; ix < items.size() + 2; ix++)
            {
                cons::printf("                          \n");
            }
            if (key == 'a') 
            {	// 実行
                if (items[select].funcA != nullptr)
                {
                    cons::setRowCol(row, col);
                    key = items[select].funcA(select, items[select].any);
                }
                continue;
            }
            // 'c' のとき
			break;
		}
		return key;
	}
}
