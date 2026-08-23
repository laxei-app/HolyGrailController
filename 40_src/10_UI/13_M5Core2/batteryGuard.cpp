// M5Stack Core2 の電源断判定(batt::offJudge)。
// 判定の理屈(連続 offConfirm 回下回り)は CoreS3 と同じでよい。しきい値は batteryParams.h 側で持つので、
// ここを再利用しても Core2 の値で動く。
// ※ Core2 の末期の電圧降下が CoreS3 と違う振る舞いをした場合は、StickS3 のようにラッチ式へ差し替える。
#include "../10_M5Stack/batteryGuard.cpp"
