package com.canon.ccapisample;

import java.nio.BufferUnderflowException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

class OrgFormatDataSet {
    private final byte[] mLvImageData;
    private final byte[] mLvInfoData;
    private final byte[] mEventData;

    OrgFormatDataSet(byte[] bytes){
        byte[] lvImageData = null;
        byte[] lvInfoData = null;
        byte[] eventData = null;

        if(bytes != null) {
            ByteBuffer byteBuffer = ByteBuffer.wrap(bytes);
            byteBuffer.order(ByteOrder.BIG_ENDIAN);

            try {
                while (byteBuffer.position() < byteBuffer.limit()) {
                    // Start Byte (2bytes)
                    short startByte = byteBuffer.getShort();

                    if (startByte == -256) { // 0xFF00
                        // Data Type (1byte)
                        byte dataType = byteBuffer.get();

                        // Data Size (4bytes)
                        int dataSize = byteBuffer.getInt();

                        // Data
                        byte[] data = new byte[dataSize];
                        byteBuffer.get(data, 0, dataSize);

                        // End Byte (2bytes)
                        short endByte = byteBuffer.getShort();

                        switch (dataType) {
                            case 0x00:
                                lvImageData = data;
                                break;
                            case 0x01:
                                lvInfoData = data;
                                break;
                            case 0x02:
                                eventData = data;
                                break;
                            default:
                                break;
                        }

                        if (endByte != -1) { // 0xFFFF
                            break;
                        }
                    }
                    else {
                        break;
                    }
                }
            }
            catch (BufferUnderflowException e) {
                e.printStackTrace();
            }
        }

        mLvImageData = lvImageData;
        mLvInfoData = lvInfoData;
        mEventData = eventData;
    }

    byte[] getLvImageData() {
        return mLvImageData;
    }

    byte[] getLvInfoData() {
        return mLvInfoData;
    }

    byte[] getEventData() {
        return mEventData;
    }

    boolean isUnknownFormat() {
        boolean isUnknown = false;
        if(mLvImageData == null && mLvInfoData == null && mEventData == null){
            isUnknown = true;
        }
        return isUnknown;
    }

}
