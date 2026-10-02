package app.laxei.twylapse

// 【レンズがそのカメラに付くか(2026-10-03 依頼)】判断の根拠は**マスタ master/mounts.json**。
//  コードにメーカー名やマウント名を書かない — ソニーやニコンを入れるたびに手を入れる
//  ことになるため。ここでも「表に何が書いてあればどうなるか」だけを試す。
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.json.JSONArray

class LensMountTest {

    // 実際のマスタと同じ形。Canon RF / EF-M と Sony E。どれも EF レンズを受ける。
    private val rules = JSONArray(
        """[
             {"mount":"RF",  "maker":"Canon","accepts":["EF"]},
             {"mount":"EF-M","maker":"Canon","accepts":["EF"]},
             {"mount":"E",   "maker":"Sony", "accepts":["EF"]}
           ]"""
    )

    @Test
    fun 同じマウントは表に無くても付く() {
        assertTrue(GearMaster.lensFitsMount(rules, "RF", "RF"))
        assertTrue(GearMaster.lensFitsMount(rules, "E", "E"))
        // 表に行が無いマウントでも、同じ名前なら付く
        assertTrue(GearMaster.lensFitsMount(rules, "Z", "Z"))
    }

    @Test
    fun 表に書いた組み合わせはアダプタで付く() {
        assertTrue(GearMaster.lensFitsMount(rules, "RF", "EF"))
        assertTrue(GearMaster.lensFitsMount(rules, "EF-M", "EF"))
        assertTrue(GearMaster.lensFitsMount(rules, "E", "EF"))     // ソニー E にもキヤノン EF は付く
    }

    @Test
    fun 表に無い組み合わせは付かない() {
        assertFalse(GearMaster.lensFitsMount(rules, "RF", "E"))    // Canon RF と Sony E に互換は無い
        assertFalse(GearMaster.lensFitsMount(rules, "E", "RF"))
        assertFalse(GearMaster.lensFitsMount(rules, "EF-M", "RF"))
        assertFalse(GearMaster.lensFitsMount(rules, "RF", "EF-M"))
    }

    @Test
    fun 表に行が無いカメラは同じ名前のものしか付かない() {
        assertFalse(GearMaster.lensFitsMount(rules, "Z", "F"))     // Nikon を足すまでは付かない
    }

    @Test
    fun 分からないものは出す() {
        // 【消さない】手で足したレンズやカメラはマウントが空。これを消すと
        //  「登録したはずのレンズが消えた」ように見える。
        assertTrue(GearMaster.lensFitsMount(rules, "RF", ""))
        assertTrue(GearMaster.lensFitsMount(rules, "", "RF"))
        assertTrue(GearMaster.lensFitsMount(rules, "", ""))
    }

    @Test
    fun 表が無くても落ちない() {
        // マスタが古くて mounts.json を持っていないとき。同じマウントだけ付く扱いになる。
        assertTrue(GearMaster.lensFitsMount(null, "RF", "RF"))
        assertFalse(GearMaster.lensFitsMount(null, "RF", "EF"))
        assertTrue(GearMaster.lensFitsMount(null, "RF", ""))
    }

    @Test
    fun 大文字小文字は区別しない() {
        assertTrue(GearMaster.lensFitsMount(rules, "rf", "RF"))
        assertTrue(GearMaster.lensFitsMount(rules, "RF", "ef"))
    }
}
