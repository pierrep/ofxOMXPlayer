#pragma once

#include "ofMain.h"

#include "VideoDecoderBase.h"
#include "OMXDisplay.h"


class VideoDecoderNonTextured : public VideoDecoderBase
{
public:
  
    VideoDecoderNonTextured();
    ~VideoDecoderNonTextured();
    bool open(OMXStreamInfo& streamInfo,
              OMXClock *clock,
              bool deinterlace = false, 
              bool hdmi_clock_sync = false);
    
    bool decode(uint8_t *pData, int iSize, double pts);
  
    void setDisplayRect(ofRectangle& rectangle);
    void updateFrameCount();
    void onUpdate(ofEventArgs& args);
    
    int getCurrentFrame();
    void resetFrameCounter();
    
    Component imageFXComponent;
    Tunnel imageFXTunnel;
    bool doDeinterlace;
    bool doHDMISync;
    ofRectangle displayRect;
    
    OMXDisplay display;
    
private:
    int frameCounter;
    int frameOffset;
    
    
};