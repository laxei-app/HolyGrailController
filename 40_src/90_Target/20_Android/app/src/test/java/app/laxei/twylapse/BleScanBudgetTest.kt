package app.laxei.twylapse

import org.junit.Before
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

// BLE の検索回数の見張りの試験。
//
// Android は 30秒に5回を超えると検索を始めてくれず、こちらには「見つからない」と
// しか見えない。押す前に数えて待つよう伝えるのが目的なので、
// 「5回目までは通す / 6回目は止める」が守れているかを見る。
class BleScanBudgetTest {

    // 数えは1つしかないので、試験ごとに白紙へ戻す(順番に左右されないように)
    @Before fun clean() = BleScanBudget.reset()

    @Test
    fun 五回目までは始めてよい() {
        repeat(5) {
            assertEquals("%d 回目で止めてはいけない".format(it + 1), 0L, BleScanBudget.waitMs())
            BleScanBudget.record()
        }
    }

    @Test
    fun 六回目は待たせる() {
        repeat(6) { BleScanBudget.record() }
        val w = BleScanBudget.waitMs()
        assertTrue("待つよう言っていない", w > 0)
        assertTrue("待ち時間が長すぎる (%d ms)".format(w), w <= 30_000)
    }

    @Test
    // 文言はリソースへ移した(2026-10-01)ので、ここで見るのは**秒への丸め**だけ。
    fun 待ち時間は秒に丸める() {
        assertEquals(1L, BleScanBudget.waitSec(1))
        assertEquals(1L, BleScanBudget.waitSec(1000))
        assertEquals(2L, BleScanBudget.waitSec(1001))
        assertEquals(30L, BleScanBudget.waitSec(30_000))
    }
}
