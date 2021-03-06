/*
For the sake of simplicity, this example exits on error.

Very quick OpenMAX IL explanation:

- There are components. Each component performs an action. For example, the
  OMX.broadcom.camera module captures images and videos and the
  OMX.broadcom.image_encoder module encodes raw data from an image into multiple
  formats. Each component has input and output ports and receives and sends
  buffers with data. The main goal is to join these components to form a
  pipeline and do complex tasks.
- There are two ways to connect components: with tunnels or manually. The
  non-tunneled ports need to manually allocate the buffers with
  OMX_AllocateBuffer() and free them with OMX_FreeBuffer().
- The components have states.
- There are at least two threads: the thread that uses the application (CPU) and
  the thread that is used internally by OMX to execute the components (GPU).
- There are two types of functions: blocking and non-blocking. The blocking
  functions are synchronous and the non-blocking are asynchronous. Being
  asynchronous means that the function returns immediately but the result is
  returned in a later time, so you need to wait until you receive an event. This
  example uses two non-blocking functions: OMX_SendCommand and
  OMX_FillThisBuffer.

Note: The camera component has two video ports: "preview" and
"video". The "preview" port is tunneled to the video_render
component. Even if the preview would not be used it must be enabled
because it is used to run AGC (automatic gain control) and AWB (auto
white balance) algorithms.
*/

#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>

#include <bcm_host.h>
#include <interface/vcos/vcos.h>
#include <IL/OMX_Broadcom.h>

#include "dump.h"
#include <stdbool.h>

#define OMX_INIT_STRUCTURE(x) \
  memset (&(x), 0, sizeof (x)); \
  (x).nSize = sizeof (x); \
  (x).nVersion.nVersion = OMX_VERSION; \
  (x).nVersion.s.nVersionMajor = OMX_VERSION_MAJOR; \
  (x).nVersion.s.nVersionMinor = OMX_VERSION_MINOR; \
  (x).nVersion.s.nRevision = OMX_VERSION_REVISION; \
  (x).nVersion.s.nStep = OMX_VERSION_STEP

#define FILENAME "video.h264"

#define VIDEO_FRAMERATE 30
#define VIDEO_BITRATE 17000000
#define VIDEO_IDR_PERIOD 0 //Disabled
#define VIDEO_SEI OMX_FALSE
#define VIDEO_EEDE OMX_FALSE
#define VIDEO_EEDE_LOSS_RATE 0
#define VIDEO_QP OMX_FALSE
#define VIDEO_QP_I 0 //1 .. 51, 0 means off
#define VIDEO_QP_P 0 //1 .. 51, 0 means off
#define VIDEO_PROFILE OMX_VIDEO_AVCProfileHigh
#define VIDEO_INLINE_HEADERS OMX_FALSE

//Some settings doesn't work well
#define CAM_WIDTH 1920
#define CAM_HEIGHT 1080
#define CAM_SHARPNESS 0 //-100 .. 100
#define CAM_CONTRAST 0 //-100 .. 100
#define CAM_BRIGHTNESS 50 //0 .. 100
#define CAM_SATURATION 0 //-100 .. 100
#define CAM_SHUTTER_SPEED_AUTO OMX_TRUE
#define CAM_SHUTTER_SPEED 1.0/8.0
#define CAM_ISO_AUTO OMX_TRUE
#define CAM_ISO 100 //100 .. 800
#define CAM_EXPOSURE OMX_ExposureControlAuto
#define CAM_EXPOSURE_COMPENSATION 0 //-24 .. 24
#define CAM_MIRROR OMX_MirrorNone
#define CAM_ROTATION 0 //0 90 180 270
#define CAM_COLOR_ENABLE OMX_FALSE
#define CAM_COLOR_U 128 //0 .. 255
#define CAM_COLOR_V 128 //0 .. 255
#define CAM_NOISE_REDUCTION OMX_TRUE
#define CAM_FRAME_STABILIZATION OMX_FALSE
#define CAM_METERING OMX_MeteringModeAverage
#define CAM_WHITE_BALANCE OMX_WhiteBalControlAuto
//The gains are used if the white balance is set to off
#define CAM_WHITE_BALANCE_RED_GAIN 1000 //0 ..
#define CAM_WHITE_BALANCE_BLUE_GAIN 1000 //0 ..
#define CAM_IMAGE_FILTER OMX_ImageFilterNone
#define CAM_ROI_TOP 0 //0 .. 100
#define CAM_ROI_LEFT 0 //0 .. 100
#define CAM_ROI_WIDTH 100 //0 .. 100
#define CAM_ROI_HEIGHT 100 //0 .. 100
#define CAM_DRC OMX_DynRangeExpOff

#define HS_RECORD
#define HS_INTERCEPT

static volatile int sKeepRunning = 1;

void intHandler(int dummy)
{
  sKeepRunning = 0;
}

/*
Possible values:

CAM_EXPOSURE
  OMX_ExposureControlOff
  OMX_ExposureControlAuto
  OMX_ExposureControlNight
  OMX_ExposureControlBackLight
  OMX_ExposureControlSpotlight
  OMX_ExposureControlSports
  OMX_ExposureControlSnow
  OMX_ExposureControlBeach
  OMX_ExposureControlLargeAperture
  OMX_ExposureControlSmallAperture
  OMX_ExposureControlVeryLong
  OMX_ExposureControlFixedFps
  OMX_ExposureControlNightWithPreview
  OMX_ExposureControlAntishake
  OMX_ExposureControlFireworks

CAM_IMAGE_FILTER
  OMX_ImageFilterNone
  OMX_ImageFilterEmboss
  OMX_ImageFilterNegative
  OMX_ImageFilterSketch
  OMX_ImageFilterOilPaint
  OMX_ImageFilterHatch
  OMX_ImageFilterGpen
  OMX_ImageFilterSolarize
  OMX_ImageFilterWatercolor
  OMX_ImageFilterPastel
  OMX_ImageFilterFilm
  OMX_ImageFilterBlur
  OMX_ImageFilterColourSwap
  OMX_ImageFilterWashedOut
  OMX_ImageFilterColourPoint
  OMX_ImageFilterPosterise
  OMX_ImageFilterColourBalance
  OMX_ImageFilterCartoon

CAM_METERING
  OMX_MeteringModeAverage
  OMX_MeteringModeSpot
  OMX_MeteringModeMatrix
  OMX_MeteringModeBacklit

CAM_MIRROR
  OMX_MirrorNone
  OMX_MirrorHorizontal
  OMX_MirrorVertical
  OMX_MirrorBoth

CAM_WHITE_BALANCE
  OMX_WhiteBalControlOff
  OMX_WhiteBalControlAuto
  OMX_WhiteBalControlSunLight
  OMX_WhiteBalControlCloudy
  OMX_WhiteBalControlShade
  OMX_WhiteBalControlTungsten
  OMX_WhiteBalControlFluorescent
  OMX_WhiteBalControlIncandescent
  OMX_WhiteBalControlFlash
  OMX_WhiteBalControlHorizon

CAM_DRC
  OMX_DynRangeExpOff
  OMX_DynRangeExpLow
  OMX_DynRangeExpMedium
  OMX_DynRangeExpHigh

VIDEO_PROFILE
  OMX_VIDEO_AVCProfileHigh
  OMX_VIDEO_AVCProfileBaseline
  OMX_VIDEO_AVCProfileMain
*/

//Data of each component
typedef struct {
  //The handle is obtained with OMX_GetHandle() and is used on every function
  //that needs to manipulate a component. It is released with OMX_FreeHandle()
  OMX_HANDLETYPE handle;
  //Bitwise OR of flags. Used for blocking the current thread and waiting an
  //event. Used with vcos_event_flags_get() and vcos_event_flags_set()
  VCOS_EVENT_FLAGS_T flags;
  //The fullname of the component
  OMX_STRING name;
} component_t;

//Events used with vcos_event_flags_get() and vcos_event_flags_set()
typedef enum {
  EVENT_ERROR = 0x1,
  EVENT_PORT_ENABLE = 0x2,
  EVENT_PORT_DISABLE = 0x4,
  EVENT_STATE_SET = 0x8,
  EVENT_FLUSH = 0x10,
  EVENT_MARK_BUFFER = 0x20,
  EVENT_MARK = 0x40,
  EVENT_PORT_SETTINGS_CHANGED = 0x80,
  EVENT_PARAM_OR_CONFIG_CHANGED = 0x100,
  EVENT_BUFFER_FLAG = 0x200,
  EVENT_RESOURCES_ACQUIRED = 0x400,
  EVENT_DYNAMIC_RESOURCES_AVAILABLE = 0x800,
  EVENT_FILL_BUFFER_DONE = 0x1000,
  EVENT_EMPTY_BUFFER_DONE = 0x2000,
} component_event;

//Prototypes
OMX_ERRORTYPE event_handler (
    OMX_IN OMX_HANDLETYPE comp,
    OMX_IN OMX_PTR app_data,
    OMX_IN OMX_EVENTTYPE event,
    OMX_IN OMX_U32 data1,
    OMX_IN OMX_U32 data2,
    OMX_IN OMX_PTR event_data);
OMX_ERRORTYPE fill_buffer_done (
    OMX_IN OMX_HANDLETYPE comp,
    OMX_IN OMX_PTR app_data,
    OMX_IN OMX_BUFFERHEADERTYPE* buffer);
void wake (component_t* component, VCOS_UNSIGNED event);
void wait (
    component_t* component,
    VCOS_UNSIGNED events,
    VCOS_UNSIGNED* retrieved_events);
void init_component (component_t* component);
void deinit_component (component_t* component);
void load_camera_drivers (component_t* component);
void change_state (component_t* component, OMX_STATETYPE state);
void enable_port (component_t* component, OMX_U32 port);
void disable_port (component_t* component, OMX_U32 port);
void enable_encoder_output_port (
    component_t* encoder,
    OMX_BUFFERHEADERTYPE** encoder_output_buffer);
void disable_encoder_output_port (
    component_t* encoder,
    OMX_BUFFERHEADERTYPE* encoder_output_buffer);
void enable_camera_output_port (
    component_t* camera,
    OMX_BUFFERHEADERTYPE** camera_output_buffer);
void disable_camera_output_port (
    component_t* camera,
    OMX_BUFFERHEADERTYPE* encoder_output_buffer);
void set_camera_settings (component_t* camera);
void set_h264_settings (component_t* encoder);

//Function that is called when a component receives an event from a secondary
//thread
OMX_ERRORTYPE event_handler (
    OMX_IN OMX_HANDLETYPE comp,
    OMX_IN OMX_PTR app_data,
    OMX_IN OMX_EVENTTYPE event,
    OMX_IN OMX_U32 data1,
    OMX_IN OMX_U32 data2,
    OMX_IN OMX_PTR event_data){
  component_t* component = (component_t*)app_data;
  
  switch (event){
    case OMX_EventCmdComplete:
      switch (data1){
        case OMX_CommandStateSet:
          printf ("event: %s, OMX_CommandStateSet, state: %s\n",
              component->name, dump_OMX_STATETYPE (data2));
          wake (component, EVENT_STATE_SET);
          break;
        case OMX_CommandPortDisable:
          printf ("event: %s, OMX_CommandPortDisable, port: %d\n",
              component->name, data2);
          wake (component, EVENT_PORT_DISABLE);
          break;
        case OMX_CommandPortEnable:
          printf ("event: %s, OMX_CommandPortEnable, port: %d\n",
              component->name, data2);
          wake (component, EVENT_PORT_ENABLE);
          break;
        case OMX_CommandFlush:
          printf ("event: %s, OMX_CommandFlush, port: %d\n",
              component->name, data2);
          wake (component, EVENT_FLUSH);
          break;
        case OMX_CommandMarkBuffer:
          printf ("event: %s, OMX_CommandMarkBuffer, port: %d\n",
              component->name, data2);
          wake (component, EVENT_MARK_BUFFER);
          break;
      default:
	//This should never execute, just ignore
	printf ("event: OMX_EventCmdComplete unknown (%X)\n", data1);
	break;
      }
      break;
    case OMX_EventError:
      printf ("event: %s, %s\n", component->name, dump_OMX_ERRORTYPE (data1));
      wake (component, EVENT_ERROR);
      break;
    case OMX_EventMark:
      printf ("event: %s, OMX_EventMark\n", component->name);
      wake (component, EVENT_MARK);
      break;
    case OMX_EventPortSettingsChanged:
      printf ("event: %s, OMX_EventPortSettingsChanged, port: %d\n",
          component->name, data1);
      wake (component, EVENT_PORT_SETTINGS_CHANGED);
      break;
    case OMX_EventParamOrConfigChanged:
      printf ("event: %s, OMX_EventParamOrConfigChanged, data1: %d, data2: "
          "%X\n", component->name, data1, data2);
      wake (component, EVENT_PARAM_OR_CONFIG_CHANGED);
      break;
    case OMX_EventBufferFlag:
      printf ("event: %s, OMX_EventBufferFlag, port: %d\n",
          component->name, data1);
      wake (component, EVENT_BUFFER_FLAG);
      break;
    case OMX_EventResourcesAcquired:
      printf ("event: %s, OMX_EventResourcesAcquired\n", component->name);
      wake (component, EVENT_RESOURCES_ACQUIRED);
      break;
    case OMX_EventDynamicResourcesAvailable:
      printf ("event: %s, OMX_EventDynamicResourcesAvailable\n",
          component->name);
      wake (component, EVENT_DYNAMIC_RESOURCES_AVAILABLE);
      break;
    default:
      //This should never execute, just ignore
      printf ("event: unknown (%X)\n", event);
      break;
  }

  return OMX_ErrorNone;
}

//Function that is called when a component fills a buffer with data
OMX_ERRORTYPE fill_buffer_done (
    OMX_IN OMX_HANDLETYPE comp,
    OMX_IN OMX_PTR app_data,
    OMX_IN OMX_BUFFERHEADERTYPE* buffer){
  component_t* component = (component_t*)app_data;
  
  printf ("event: %s, fill_buffer_done\n", component->name);
  wake (component, EVENT_FILL_BUFFER_DONE);
  
  return OMX_ErrorNone;
}

//Function that is called when a component fills a buffer with data
OMX_ERRORTYPE empty_buffer_done (
    OMX_IN OMX_HANDLETYPE comp,
    OMX_IN OMX_PTR app_data,
    OMX_IN OMX_BUFFERHEADERTYPE* buffer){
  component_t* component = (component_t*)app_data;
  
  printf ("event: %s, empty_buffer_done\n", component->name);
  wake (component, EVENT_EMPTY_BUFFER_DONE);
  
  return OMX_ErrorNone;
}

void wake (component_t* component, VCOS_UNSIGNED event){
  vcos_event_flags_set (&component->flags, event, VCOS_OR);
}

void wait (
    component_t* component,
    VCOS_UNSIGNED events,
    VCOS_UNSIGNED* retrieved_events){
  VCOS_UNSIGNED set;
  if (vcos_event_flags_get (&component->flags, events | EVENT_ERROR,
      VCOS_OR_CONSUME, VCOS_SUSPEND, &set)){
    fprintf (stderr, "error: vcos_event_flags_get\n");
    exit (1);
  }
  if (set == EVENT_ERROR){
    exit (1);
  }
  if (retrieved_events){
    *retrieved_events = set;
  }
}

void init_component (component_t* component){
  printf ("initializing component %s\n", component->name);
  
  OMX_ERRORTYPE error;
  
  //Create the event flags
  if (vcos_event_flags_create (&component->flags, "component")){
    fprintf (stderr, "error: vcos_event_flags_create\n");
    exit (1);
  }
  
  //Each component has an event_handler and fill_buffer_done functions
  OMX_CALLBACKTYPE callbacks_st;
  callbacks_st.EventHandler = event_handler;
  callbacks_st.FillBufferDone = fill_buffer_done;
  callbacks_st.EmptyBufferDone = empty_buffer_done;
  
  //Get the handle
  if ((error = OMX_GetHandle (&component->handle, component->name, component,
      &callbacks_st))){
    fprintf (stderr, "error: OMX_GetHandle: %s\n", dump_OMX_ERRORTYPE (error));
    exit (1);
  }
  
  //Disable all the ports
  OMX_INDEXTYPE types[] = {
    OMX_IndexParamAudioInit,
    OMX_IndexParamVideoInit,
    OMX_IndexParamImageInit,
    OMX_IndexParamOtherInit
  };
  OMX_PORT_PARAM_TYPE ports_st;
  OMX_INIT_STRUCTURE (ports_st);

  int i;
  for (i=0; i<4; i++){
    if ((error = OMX_GetParameter (component->handle, types[i], &ports_st))){
      fprintf (stderr, "error: OMX_GetParameter: %s\n",
          dump_OMX_ERRORTYPE (error));
      exit (1);
    }
    
    OMX_U32 port;
    for (port=ports_st.nStartPortNumber;
        port<ports_st.nStartPortNumber + ports_st.nPorts; port++){
      //Disable the port
      disable_port (component, port);
      //Wait to the event
      wait (component, EVENT_PORT_DISABLE, 0);
    }
  }
}

void deinit_component (component_t* component){
  printf ("deinitializing component %s\n", component->name);
  
  OMX_ERRORTYPE error;
  
  vcos_event_flags_delete (&component->flags);

  if ((error = OMX_FreeHandle (component->handle))){
    fprintf (stderr, "error: OMX_FreeHandle: %s\n", dump_OMX_ERRORTYPE (error));
    exit (1);
  }
}

void load_camera_drivers (component_t* component){
  /*
  This is a specific behaviour of the Broadcom's Raspberry Pi OpenMAX IL
  implementation module because the OMX_SetConfig() and OMX_SetParameter() are
  blocking functions but the drivers are loaded asynchronously, that is, an
  event is fired to signal the completion. Basically, what you're saying is:
  
  "When the parameter with index OMX_IndexParamCameraDeviceNumber is set, load
  the camera drivers and emit an OMX_EventParamOrConfigChanged event"
  
  The red LED of the camera will be turned on after this call.
  */
  
  printf ("loading camera drivers\n");
  
  OMX_ERRORTYPE error;

  OMX_CONFIG_REQUESTCALLBACKTYPE cbs_st;
  OMX_INIT_STRUCTURE (cbs_st);
  cbs_st.nPortIndex = OMX_ALL;
  cbs_st.nIndex = OMX_IndexParamCameraDeviceNumber;
  cbs_st.bEnable = OMX_TRUE;
  if ((error = OMX_SetConfig (component->handle, OMX_IndexConfigRequestCallback,
      &cbs_st))){
    fprintf (stderr, "error: OMX_SetConfig: %s\n", dump_OMX_ERRORTYPE (error));
    exit (1);
  }
  
  OMX_PARAM_U32TYPE dev_st;
  OMX_INIT_STRUCTURE (dev_st);
  dev_st.nPortIndex = OMX_ALL;
  //ID for the camera device
  dev_st.nU32 = 0;
  if ((error = OMX_SetParameter (component->handle,
      OMX_IndexParamCameraDeviceNumber, &dev_st))){
    fprintf (stderr, "error: OMX_SetParameter: %s\n",
        dump_OMX_ERRORTYPE (error));
    exit (1);
  }
  
  wait (component, EVENT_PARAM_OR_CONFIG_CHANGED, 0);
}

void change_state (component_t* component, OMX_STATETYPE state){
  printf ("changing %s state to %s\n", component->name,
      dump_OMX_STATETYPE (state));
  
  OMX_ERRORTYPE error;
  
  if ((error = OMX_SendCommand (component->handle, OMX_CommandStateSet, state,
      0))){
    fprintf (stderr, "error: OMX_SendCommand: %s\n",
        dump_OMX_ERRORTYPE (error));
    exit (1);
  }
}

void enable_port (component_t* component, OMX_U32 port){
  printf ("enabling port %d (%s)\n", port, component->name);
  
  OMX_ERRORTYPE error;
  
  if ((error = OMX_SendCommand (component->handle, OMX_CommandPortEnable,
      port, 0))){
    fprintf (stderr, "error: OMX_SendCommand: %s\n",
        dump_OMX_ERRORTYPE (error));
    exit (1);
  }
}

void disable_port (component_t* component, OMX_U32 port){
  printf ("disabling port %d (%s)\n", port, component->name);
  
  OMX_ERRORTYPE error;
  
  if ((error = OMX_SendCommand (component->handle, OMX_CommandPortDisable,
      port, 0))){
    fprintf (stderr, "error: OMX_SendCommand: %s\n",
        dump_OMX_ERRORTYPE (error));
    exit (1);
  }
}

void enable_encoder_output_port (
    component_t* encoder,
    OMX_BUFFERHEADERTYPE** encoder_output_buffer){
  //The port is not enabled until the buffer is allocated
  OMX_ERRORTYPE error;
  
  OMX_PARAM_PORTDEFINITIONTYPE port_st;
  OMX_INIT_STRUCTURE (port_st);
  port_st.nPortIndex = 201;
  if ((error = OMX_GetParameter (encoder->handle, OMX_IndexParamPortDefinition,
      &port_st))){
    fprintf (stderr, "error: OMX_GetParameter: %s\n",
        dump_OMX_ERRORTYPE (error));
    exit (1);
  }
  printf ("allocating %s output buffer\n", encoder->name);
  if ((error = OMX_AllocateBuffer (encoder->handle, encoder_output_buffer, 201,
      0, port_st.nBufferSize))){
    fprintf (stderr, "error: OMX_AllocateBuffer: %s\n",
        dump_OMX_ERRORTYPE (error));
    exit (1);
  }
}

void disable_encoder_output_port (
    component_t* encoder,
    OMX_BUFFERHEADERTYPE* encoder_output_buffer){
  //The port is not disabled until the buffer is released
  OMX_ERRORTYPE error;
  
  //Free encoder output buffer
  printf ("releasing %s output buffer\n", encoder->name);
  if ((error = OMX_FreeBuffer (encoder->handle, 201, encoder_output_buffer))){
    fprintf (stderr, "error: OMX_FreeBuffer: %s\n", dump_OMX_ERRORTYPE (error));
    exit (1);
  }
}

void enable_camera_output_port (
    component_t* camera,
    OMX_BUFFERHEADERTYPE** camera_output_buffer){
  //The port is not enabled until the buffer is allocated
  OMX_ERRORTYPE error;
  
  OMX_PARAM_PORTDEFINITIONTYPE port_st;
  OMX_INIT_STRUCTURE (port_st);
  port_st.nPortIndex = 70;
  if ((error = OMX_GetParameter (camera->handle, OMX_IndexParamPortDefinition,
      &port_st))){
    fprintf (stderr, "error: OMX_GetParameter: %s\n",
        dump_OMX_ERRORTYPE (error));
    exit (1);
  }

  printf ("allocating %s output buffer with size %i %i (%ix%i) %i %i %i %p %p\n", camera->name, port_st.nBufferSize, port_st.eDomain, port_st.format.video.nFrameWidth, port_st.format.video.nFrameHeight, port_st.format.video.nStride, port_st.format.video.nBitrate, port_st.format.video.eCompressionFormat, port_st.format.video.pNativeRender, port_st.format.video.pNativeWindow);
#ifdef HS_INTERCEPT
  void* ptr = malloc(port_st.nBufferSize);
  /* if ((error = OMX_AllocateBuffer (camera->handle, camera_output_buffer, 70, */
  /*     0, port_st.nBufferSize))){ */
  if ((error = OMX_UseBuffer (camera->handle, camera_output_buffer, 70,
			      0, port_st.nBufferSize, ptr))){
    fprintf (stderr, "error: OMX_AllocateBuffer: %s\n",
        dump_OMX_ERRORTYPE (error));
    exit (1);
  }
#endif
}

void enable_render_input_port (
    component_t* render,
    OMX_BUFFERHEADERTYPE** render_input_buffer){
  //The port is not enabled until the buffer is allocated
  OMX_ERRORTYPE error;
  
  OMX_PARAM_PORTDEFINITIONTYPE port_st;
  OMX_INIT_STRUCTURE (port_st);
  port_st.nPortIndex = 90;
  if ((error = OMX_GetParameter (render->handle, OMX_IndexParamPortDefinition,
      &port_st))){
    fprintf (stderr, "error: OMX_GetParameter: %s\n",
        dump_OMX_ERRORTYPE (error));
    exit (1);
  }

  printf ("allocating %s input buffer with size %i %i (%ix%i)\n", render->name, port_st.nBufferSize, port_st.eDomain, port_st.format.video.nFrameWidth, port_st.format.video.nFrameHeight);

  /* void* ptr = malloc(port_st.nBufferSize); */
  /* if ((error = OMX_UseBuffer (render->handle, render_input_buffer, 90, */
  /* 			      0, port_st.nBufferSize, ptr))){ */
  printf ("bufferCountActual = %i\n", port_st.nBufferCountActual);
  for (int i = 0; i < port_st.nBufferCountActual; i++) {
      if ((error = OMX_AllocateBuffer (render->handle, render_input_buffer+i, 90,
				       0, port_st.nBufferSize))){
	fprintf (stderr, "error: OMX_UseBuffer: %s\n",
		 dump_OMX_ERRORTYPE (error));
	exit (1);
      }
  }
}

void disable_render_input_port (
    component_t* render,
    OMX_BUFFERHEADERTYPE** buffer){
  //The port is not disabled until the buffer is released
  OMX_ERRORTYPE error;
  
  //Free camera output buffer
  printf ("releasing %s output buffer\n", render->name);
  for (int i=0; i<3; ++i)
    if ((error = OMX_FreeBuffer (render->handle, 90, buffer[i]))){
      fprintf (stderr, "error: OMX_FreeBuffer: %s\n", dump_OMX_ERRORTYPE (error));
      exit (1);
    }
}


void disable_camera_output_port (
    component_t* camera,
    OMX_BUFFERHEADERTYPE* camera_output_buffer){
  //The port is not disabled until the buffer is released
  OMX_ERRORTYPE error;
  
  //Free camera output buffer
  printf ("releasing %s output buffer\n", camera->name);
  if ((error = OMX_FreeBuffer (camera->handle, 70, camera_output_buffer))){
    fprintf (stderr, "error: OMX_FreeBuffer: %s\n", dump_OMX_ERRORTYPE (error));
    exit (1);
  }
}

void set_camera_settings (component_t* camera){
  printf ("configuring '%s' settings\n", camera->name);

  OMX_ERRORTYPE error;
  
  //Sharpness
  OMX_CONFIG_SHARPNESSTYPE sharpness_st;
  OMX_INIT_STRUCTURE (sharpness_st);
  sharpness_st.nPortIndex = OMX_ALL;
  sharpness_st.nSharpness = CAM_SHARPNESS;
  if ((error = OMX_SetConfig (camera->handle, OMX_IndexConfigCommonSharpness,
      &sharpness_st))){
    fprintf (stderr, "error: OMX_SetConfig: %s\n", dump_OMX_ERRORTYPE (error));
    exit (1);
  }
  
  //Contrast
  OMX_CONFIG_CONTRASTTYPE contrast_st;
  OMX_INIT_STRUCTURE (contrast_st);
  contrast_st.nPortIndex = OMX_ALL;
  contrast_st.nContrast = CAM_CONTRAST;
  if ((error = OMX_SetConfig (camera->handle, OMX_IndexConfigCommonContrast,
      &contrast_st))){
    fprintf (stderr, "error: OMX_SetConfig: %s\n", dump_OMX_ERRORTYPE (error));
    exit (1);
  }
  
  //Saturation
  OMX_CONFIG_SATURATIONTYPE saturation_st;
  OMX_INIT_STRUCTURE (saturation_st);
  saturation_st.nPortIndex = OMX_ALL;
  saturation_st.nSaturation = CAM_SATURATION;
  if ((error = OMX_SetConfig (camera->handle, OMX_IndexConfigCommonSaturation,
      &saturation_st))){
    fprintf (stderr, "error: OMX_SetConfig: %s\n", dump_OMX_ERRORTYPE (error));
    exit (1);
  }
  
  //Brightness
  OMX_CONFIG_BRIGHTNESSTYPE brightness_st;
  OMX_INIT_STRUCTURE (brightness_st);
  brightness_st.nPortIndex = OMX_ALL;
  brightness_st.nBrightness = CAM_BRIGHTNESS;
  if ((error = OMX_SetConfig (camera->handle, OMX_IndexConfigCommonBrightness,
      &brightness_st))){
    fprintf (stderr, "error: OMX_SetConfig: %s\n", dump_OMX_ERRORTYPE (error));
    exit (1);
  }
  
  //Exposure value
  OMX_CONFIG_EXPOSUREVALUETYPE exposure_value_st;
  OMX_INIT_STRUCTURE (exposure_value_st);
  exposure_value_st.nPortIndex = OMX_ALL;
  exposure_value_st.eMetering = CAM_METERING;
  exposure_value_st.xEVCompensation =
      (OMX_S32)((CAM_EXPOSURE_COMPENSATION<<16)/6.0);
  exposure_value_st.nShutterSpeedMsec = (OMX_U32)((CAM_SHUTTER_SPEED)*1e6);
  exposure_value_st.bAutoShutterSpeed = CAM_SHUTTER_SPEED_AUTO;
  exposure_value_st.nSensitivity = CAM_ISO;
  exposure_value_st.bAutoSensitivity = CAM_ISO_AUTO;
  if ((error = OMX_SetConfig (camera->handle,
      OMX_IndexConfigCommonExposureValue, &exposure_value_st))){
    fprintf (stderr, "error: OMX_SetConfig: %s\n", dump_OMX_ERRORTYPE (error));
    exit (1);
  }
  
  //Exposure control
  OMX_CONFIG_EXPOSURECONTROLTYPE exposure_control_st;
  OMX_INIT_STRUCTURE (exposure_control_st);
  exposure_control_st.nPortIndex = OMX_ALL;
  exposure_control_st.eExposureControl = CAM_EXPOSURE;
  if ((error = OMX_SetConfig (camera->handle, OMX_IndexConfigCommonExposure,
      &exposure_control_st))){
    fprintf (stderr, "error: OMX_SetConfig: %s\n", dump_OMX_ERRORTYPE (error));
    exit (1);
  }
  
  //Frame stabilisation
  OMX_CONFIG_FRAMESTABTYPE frame_stabilisation_st;
  OMX_INIT_STRUCTURE (frame_stabilisation_st);
  frame_stabilisation_st.nPortIndex = OMX_ALL;
  frame_stabilisation_st.bStab = CAM_FRAME_STABILIZATION;
  if ((error = OMX_SetConfig (camera->handle,
      OMX_IndexConfigCommonFrameStabilisation, &frame_stabilisation_st))){
    fprintf (stderr, "error: OMX_SetConfig: %s\n", dump_OMX_ERRORTYPE (error));
    exit (1);
  }
  
  //White balance
  OMX_CONFIG_WHITEBALCONTROLTYPE white_balance_st;
  OMX_INIT_STRUCTURE (white_balance_st);
  white_balance_st.nPortIndex = OMX_ALL;
  white_balance_st.eWhiteBalControl = CAM_WHITE_BALANCE;
  if ((error = OMX_SetConfig (camera->handle, OMX_IndexConfigCommonWhiteBalance,
      &white_balance_st))){
    fprintf (stderr, "error: OMX_SetConfig: %s\n", dump_OMX_ERRORTYPE (error));
    exit (1);
  }
  
  //White balance gains (if white balance is set to off)
  if (!CAM_WHITE_BALANCE){
    OMX_CONFIG_CUSTOMAWBGAINSTYPE white_balance_gains_st;
    OMX_INIT_STRUCTURE (white_balance_gains_st);
    white_balance_gains_st.xGainR = (CAM_WHITE_BALANCE_RED_GAIN << 16)/1000;
    white_balance_gains_st.xGainB = (CAM_WHITE_BALANCE_BLUE_GAIN << 16)/1000;
    if ((error = OMX_SetConfig (camera->handle, OMX_IndexConfigCustomAwbGains,
        &white_balance_gains_st))){
      fprintf (stderr, "error: OMX_SetConfig: %s\n",
          dump_OMX_ERRORTYPE (error));
      exit (1);
    }
  }
  
  //Image filter
  OMX_CONFIG_IMAGEFILTERTYPE image_filter_st;
  OMX_INIT_STRUCTURE (image_filter_st);
  image_filter_st.nPortIndex = OMX_ALL;
  image_filter_st.eImageFilter = CAM_IMAGE_FILTER;
  if ((error = OMX_SetConfig (camera->handle, OMX_IndexConfigCommonImageFilter,
      &image_filter_st))){
    fprintf (stderr, "error: OMX_SetConfig: %s\n", dump_OMX_ERRORTYPE (error));
    exit (1);
  }
  
  //Mirror
  OMX_CONFIG_MIRRORTYPE mirror_st;
  OMX_INIT_STRUCTURE (mirror_st);
  mirror_st.nPortIndex = 71;
  mirror_st.eMirror = CAM_MIRROR;
  if ((error = OMX_SetConfig (camera->handle, OMX_IndexConfigCommonMirror,
      &mirror_st))){
    fprintf (stderr, "error: OMX_SetConfig: %s\n", dump_OMX_ERRORTYPE (error));
    exit (1);
  }
  
  //Rotation
  OMX_CONFIG_ROTATIONTYPE rotation_st;
  OMX_INIT_STRUCTURE (rotation_st);
  rotation_st.nPortIndex = 71;
  rotation_st.nRotation = CAM_ROTATION;
  if ((error = OMX_SetConfig (camera->handle, OMX_IndexConfigCommonRotate,
      &rotation_st))){
    fprintf (stderr, "error: OMX_SetConfig: %s\n", dump_OMX_ERRORTYPE (error));
    exit (1);
  }
  
  //Color enhancement
  OMX_CONFIG_COLORENHANCEMENTTYPE color_enhancement_st;
  OMX_INIT_STRUCTURE (color_enhancement_st);
  color_enhancement_st.nPortIndex = OMX_ALL;
  color_enhancement_st.bColorEnhancement = CAM_COLOR_ENABLE;
  color_enhancement_st.nCustomizedU = CAM_COLOR_U;
  color_enhancement_st.nCustomizedV = CAM_COLOR_V;
  if ((error = OMX_SetConfig (camera->handle,
      OMX_IndexConfigCommonColorEnhancement, &color_enhancement_st))){
    fprintf (stderr, "error: OMX_SetConfig: %s\n", dump_OMX_ERRORTYPE (error));
    exit (1);
  }
  
  //Denoise
  OMX_CONFIG_BOOLEANTYPE denoise_st;
  OMX_INIT_STRUCTURE (denoise_st);
  denoise_st.bEnabled = CAM_NOISE_REDUCTION;
  if ((error = OMX_SetConfig (camera->handle,
      OMX_IndexConfigStillColourDenoiseEnable, &denoise_st))){
    fprintf (stderr, "error: OMX_SetConfig: %s\n", dump_OMX_ERRORTYPE (error));
    exit (1);
  }
  
  //ROI
  OMX_CONFIG_INPUTCROPTYPE roi_st;
  OMX_INIT_STRUCTURE (roi_st);
  roi_st.nPortIndex = OMX_ALL;
  roi_st.xLeft = (CAM_ROI_LEFT << 16)/100;
  roi_st.xTop = (CAM_ROI_TOP << 16)/100;
  roi_st.xWidth = (CAM_ROI_WIDTH << 16)/100;
  roi_st.xHeight = (CAM_ROI_HEIGHT << 16)/100;
  if ((error = OMX_SetConfig (camera->handle,
      OMX_IndexConfigInputCropPercentages, &roi_st))){
    fprintf (stderr, "error: OMX_SetConfig: %s\n", dump_OMX_ERRORTYPE (error));
    exit (1);
  }
  
  //DRC
  OMX_CONFIG_DYNAMICRANGEEXPANSIONTYPE drc_st;
  OMX_INIT_STRUCTURE (drc_st);
  drc_st.eMode = CAM_DRC;
  if ((error = OMX_SetConfig (camera->handle,
      OMX_IndexConfigDynamicRangeExpansion, &drc_st))){
    fprintf (stderr, "error: OMX_SetConfig: %s\n", dump_OMX_ERRORTYPE (error));
    exit (1);
  }
}

void set_h264_settings (component_t* encoder){
  printf ("configuring '%s' settings\n", encoder->name);
  
  OMX_ERRORTYPE error;
  
  if (!VIDEO_QP){
    //Bitrate
    OMX_VIDEO_PARAM_BITRATETYPE bitrate_st;
    OMX_INIT_STRUCTURE (bitrate_st);
    bitrate_st.eControlRate = OMX_Video_ControlRateVariable;
    bitrate_st.nTargetBitrate = VIDEO_BITRATE;
    bitrate_st.nPortIndex = 201;
    if ((error = OMX_SetParameter (encoder->handle, OMX_IndexParamVideoBitrate,
        &bitrate_st))){
      fprintf (stderr, "error: OMX_SetParameter: %s\n",
          dump_OMX_ERRORTYPE (error));
      exit (1);
    }
  }else{
    //Quantization parameters
    OMX_VIDEO_PARAM_QUANTIZATIONTYPE quantization_st;
    OMX_INIT_STRUCTURE (quantization_st);
    quantization_st.nPortIndex = 201;
    //nQpB returns an error, it cannot be modified
    quantization_st.nQpI = VIDEO_QP_I;
    quantization_st.nQpP = VIDEO_QP_P;
    if ((error = OMX_SetParameter (encoder->handle,
        OMX_IndexParamVideoQuantization, &quantization_st))){
      fprintf (stderr, "error: OMX_SetParameter: %s\n",
          dump_OMX_ERRORTYPE (error));
      exit (1);
    }
  }
  
  //Codec
  OMX_VIDEO_PARAM_PORTFORMATTYPE format_st;
  OMX_INIT_STRUCTURE (format_st);
  format_st.nPortIndex = 201;
  //H.264/AVC
  format_st.eCompressionFormat = OMX_VIDEO_CodingAVC;
  if ((error = OMX_SetParameter (encoder->handle, OMX_IndexParamVideoPortFormat,
      &format_st))){
    fprintf (stderr, "error: OMX_SetParameter: %s\n",
        dump_OMX_ERRORTYPE (error));
    exit (1);
  }
  
  //IDR period
  OMX_VIDEO_CONFIG_AVCINTRAPERIOD idr_st;
  OMX_INIT_STRUCTURE (idr_st);
  idr_st.nPortIndex = 201;
  if ((error = OMX_GetConfig (encoder->handle,
      OMX_IndexConfigVideoAVCIntraPeriod, &idr_st))){
    fprintf (stderr, "error: OMX_GetConfig: %s\n", dump_OMX_ERRORTYPE (error));
    exit (1);
  }
  idr_st.nIDRPeriod = VIDEO_IDR_PERIOD;
  if ((error = OMX_SetConfig (encoder->handle,
      OMX_IndexConfigVideoAVCIntraPeriod, &idr_st))){
    fprintf (stderr, "error: OMX_SetConfig: %s\n", dump_OMX_ERRORTYPE (error));
    exit (1);
  }
  
  //SEI
  OMX_PARAM_BRCMVIDEOAVCSEIENABLETYPE sei_st;
  OMX_INIT_STRUCTURE (sei_st);
  sei_st.nPortIndex = 201;
  sei_st.bEnable = VIDEO_SEI;
  if ((error = OMX_SetParameter (encoder->handle,
      OMX_IndexParamBrcmVideoAVCSEIEnable, &sei_st))){
    fprintf (stderr, "error: OMX_SetParameter: %s\n",
        dump_OMX_ERRORTYPE (error));
    exit (1);
  }
  
  //EEDE
  OMX_VIDEO_EEDE_ENABLE eede_st;
  OMX_INIT_STRUCTURE (eede_st);
  eede_st.nPortIndex = 201;
  eede_st.enable = VIDEO_EEDE;
  if ((error = OMX_SetParameter (encoder->handle, OMX_IndexParamBrcmEEDEEnable,
      &eede_st))){
    fprintf (stderr, "error: OMX_SetParameter: %s\n",
        dump_OMX_ERRORTYPE (error));
    exit (1);
  }
  
  OMX_VIDEO_EEDE_LOSSRATE eede_loss_rate_st;
  OMX_INIT_STRUCTURE (eede_loss_rate_st);
  eede_loss_rate_st.nPortIndex = 201;
  eede_loss_rate_st.loss_rate = VIDEO_EEDE_LOSS_RATE;
  if ((error = OMX_SetParameter (encoder->handle,
      OMX_IndexParamBrcmEEDELossRate, &eede_loss_rate_st))){
    fprintf (stderr, "error: OMX_SetParameter: %s\n",
        dump_OMX_ERRORTYPE (error));
    exit (1);
  }
  
  //AVC Profile
  OMX_VIDEO_PARAM_AVCTYPE avc_st;
  OMX_INIT_STRUCTURE (avc_st);
  avc_st.nPortIndex = 201;
  if ((error = OMX_GetParameter (encoder->handle,
      OMX_IndexParamVideoAvc, &avc_st))){
    fprintf (stderr, "error: OMX_GetParameter: %s\n",
        dump_OMX_ERRORTYPE (error));
    exit (1);
  }
  avc_st.eProfile = VIDEO_PROFILE;
  if ((error = OMX_SetParameter (encoder->handle,
      OMX_IndexParamVideoAvc, &avc_st))){
    fprintf (stderr, "error: OMX_SetParameter: %s\n",
        dump_OMX_ERRORTYPE (error));
    exit (1);
  }
  
  //Inline SPS/PPS
  OMX_CONFIG_PORTBOOLEANTYPE headers_st;
  OMX_INIT_STRUCTURE (headers_st);
  headers_st.nPortIndex = 201;
  headers_st.bEnabled = VIDEO_INLINE_HEADERS;
  if ((error = OMX_SetParameter (encoder->handle,
      OMX_IndexParamBrcmVideoAVCInlineHeaderEnable, &headers_st))){
    fprintf (stderr, "error: OMX_SetParameter: %s\n",
        dump_OMX_ERRORTYPE (error));
    exit (1);
  }
  
  //Note: Motion vectors are not implemented in this program.
  //See for further details
  //https://github.com/gagle/raspberrypi-omxcam/blob/master/src/h264.c
  //https://github.com/gagle/raspberrypi-omxcam/blob/master/src/video.c
}

void set_render_config(component_t* render)
{
  OMX_ERRORTYPE error;
  
  /* OMX_CONFIG_BRCMUSEPROPRIETARYCALLBACKTYPE port_bool; */
  /* OMX_INIT_STRUCTURE (port_bool); */
  /* port_bool.nPortIndex = 90; */
  /* if ((error = OMX_GetConfig (render->handle, OMX_IndexConfigBrcmUseProprietaryCallback, */
  /*     &port_bool))){ */
  /*   fprintf (stderr, "error: OMX_GetConfig: %s\n", dump_OMX_ERRORTYPE (error)); */
  /*   exit (1); */
  /* } */

  /* printf("proprietary = %i\n", port_bool.bEnable); */
  
  OMX_CONFIG_DISPLAYREGIONTYPE port_dispreg;
  //Configure render port definition
  printf ("configuring %s display region\n", render->name);
  OMX_INIT_STRUCTURE (port_dispreg);
  port_dispreg.nPortIndex = 90;

  printf("dispreg set = 0x%x num = %i fs = %i src = %i %i %i %i dst = %i %i %i %i mode = %x layer = %i ver = %x size = %i\n", port_dispreg.set, port_dispreg.num, port_dispreg.fullscreen, port_dispreg.src_rect.x_offset, port_dispreg.src_rect.y_offset, port_dispreg.src_rect.width, port_dispreg.src_rect.height, port_dispreg.dest_rect.x_offset, port_dispreg.dest_rect.y_offset, port_dispreg.dest_rect.width, port_dispreg.dest_rect.height, port_dispreg.mode, port_dispreg.layer, port_dispreg.nVersion.nVersion, port_dispreg.nSize);
  
  if ((error = OMX_GetConfig (render->handle, OMX_IndexConfigDisplayRegion,
      &port_dispreg))){
    fprintf (stderr, "error: OMX_GetParameter: %s\n",
        dump_OMX_ERRORTYPE (error));
    exit (1);
  }

  printf("dispreg set = 0x%x num = %i fs = %i src = %i %i %i %i dst = %i %i %i %i mode = %x layer = %i ver = %x size = %i\n", port_dispreg.set, port_dispreg.num, port_dispreg.fullscreen, port_dispreg.src_rect.x_offset, port_dispreg.src_rect.y_offset, port_dispreg.src_rect.width, port_dispreg.src_rect.height, port_dispreg.dest_rect.x_offset, port_dispreg.dest_rect.y_offset, port_dispreg.dest_rect.width, port_dispreg.dest_rect.height, port_dispreg.mode, port_dispreg.layer, port_dispreg.nVersion.nVersion, port_dispreg.nSize);
  
  port_dispreg.dest_rect.width = 1920;
  port_dispreg.dest_rect.height = 1080;
  port_dispreg.set = (OMX_DISPLAYSETTYPE)OMX_DISPLAY_SET_DEST_RECT;

  //memset(&port_dispreg, 0, sizeof(OMX_CONFIG_DISPLAYREGIONTYPE));
   port_dispreg.nSize = sizeof(OMX_CONFIG_DISPLAYREGIONTYPE);
   port_dispreg.nVersion.nVersion = OMX_VERSION;
   //  port_dispreg.nPortIndex = 90;
   //   port_dispreg.fullscreen = OMX_FALSE;
   //   port_dispreg.noaspect   = OMX_TRUE;
   //   port_dispreg.set = (OMX_DISPLAYSETTYPE)(OMX_DISPLAY_SET_DEST_RECT|OMX_DISPLAY_SET_SRC_RECT|OMX_DISPLAY_SET_FULLSCREEN|OMX_DISPLAY_SET_NOASPECT);
   //   port_dispreg.dest_rect.x_offset  = 50;
   //   port_dispreg.dest_rect.y_offset  = 50;
   //  port_dispreg.dest_rect.width     = 1280;
   //   port_dispreg.dest_rect.height    = 720;
   //   port_dispreg.src_rect.x_offset   = 0;
   //   port_dispreg.src_rect.y_offset   = 0;
   //   port_dispreg.src_rect.width      = 1280;
   //   port_dispreg.src_rect.height     = 720;
  
  printf("dispreg set = 0x%x num = %i fs = %i src = %i %i %i %i dst = %i %i %i %i mode = %x layer = %i ver = %x size = %i\n", port_dispreg.set, port_dispreg.num, port_dispreg.fullscreen, port_dispreg.src_rect.x_offset, port_dispreg.src_rect.y_offset, port_dispreg.src_rect.width, port_dispreg.src_rect.height, port_dispreg.dest_rect.x_offset, port_dispreg.dest_rect.y_offset, port_dispreg.dest_rect.width, port_dispreg.dest_rect.height, port_dispreg.mode, port_dispreg.layer, port_dispreg.nVersion.nVersion, port_dispreg.nSize);
  
  if ((error = OMX_SetConfig(render->handle, OMX_IndexConfigDisplayRegion,
      &port_dispreg))){
    fprintf (stderr, "error: OMX_SetConfig: %s\n",
        dump_OMX_ERRORTYPE (error));
    exit (1);
  }
}

void dbg_print_render(component_t* render)
{ // print some stuff to get some insights
  OMX_ERRORTYPE error;
  OMX_PARAM_PORTDEFINITIONTYPE port_st;
  OMX_INIT_STRUCTURE (port_st);
  port_st.nPortIndex = 90;
  if ((error = OMX_GetParameter (render->handle, OMX_IndexParamPortDefinition,
				 &port_st))){
    fprintf (stderr, "error: OMX_GetParameter: %s\n",
	     dump_OMX_ERRORTYPE (error));
    exit (1);
  }

  printf ("DBG %s %i %i port %i %i with size %i %i %i %i (%s (%ix%i) %i %i %i 0x%x %i 0x%x 0x%x %p %p) %i %i\n",
	  render->name,
	  port_st.nSize,
	  port_st.eDir,
	  port_st.nBufferCountActual,
	  port_st.nBufferCountMin,
	  port_st.nBufferSize,
	  port_st.bEnabled,
	  port_st.bPopulated,
	  port_st.eDomain,
	  port_st.format.video.cMIMEType,
	  port_st.format.video.nFrameWidth,
	  port_st.format.video.nFrameHeight,
	  port_st.format.video.nStride,
	  port_st.format.video.nSliceHeight,
	  port_st.format.video.nBitrate,
	  port_st.format.video.xFramerate,	  
	  port_st.format.video.bFlagErrorConcealment,	  
	  port_st.format.video.eCompressionFormat,
	  port_st.format.video.eColorFormat,
	  port_st.format.video.pNativeRender,
	  port_st.format.video.pNativeWindow,
	  port_st.bBuffersContiguous,
	  port_st.nBufferAlignment);

  OMX_CONFIG_DISPLAYREGIONTYPE port_dispreg;
  //Configure render port definition
  printf ("configuring %s display region\n", render->name);
  OMX_INIT_STRUCTURE (port_dispreg);
  port_dispreg.nPortIndex = 90;

  if ((error = OMX_GetConfig (render->handle, OMX_IndexConfigDisplayRegion,
      &port_dispreg))){
    fprintf (stderr, "error: OMX_GetParameter: %s\n",
        dump_OMX_ERRORTYPE (error));
    exit (1);
  }

  printf("dispreg size = %i ver = %x set = 0x%x num = %i fs = %i %i dst = %i %i %i %i src = %i %i %i %i %i mode = %x %i %i layer = %i %i %i %i %i\n",
	 port_dispreg.nSize,
	 port_dispreg.nVersion.nVersion,
	 port_dispreg.set,
	 port_dispreg.num,
	 port_dispreg.fullscreen,
      	 port_dispreg.transform,
	 port_dispreg.dest_rect.x_offset,
	 port_dispreg.dest_rect.y_offset,
	 port_dispreg.dest_rect.width,
	 port_dispreg.dest_rect.height,
	 port_dispreg.src_rect.x_offset,
	 port_dispreg.src_rect.y_offset,
	 port_dispreg.src_rect.width,
	 port_dispreg.src_rect.height,
	 port_dispreg.noaspect,
	 port_dispreg.mode,
	 port_dispreg.pixel_x,
	 port_dispreg.pixel_y,
	 port_dispreg.layer,
	 port_dispreg.copyprotect_required,
	 port_dispreg.alpha,
	 port_dispreg.wfc_context_width,
	 port_dispreg.wfc_context_height);
}

int main (){
  OMX_ERRORTYPE error;
  OMX_BUFFERHEADERTYPE* encoder_output_buffer;
#ifdef HS_INTERCEPT
  OMX_BUFFERHEADERTYPE* camera_output_buffer;
  OMX_BUFFERHEADERTYPE* render_input_buffer[3];
#endif
  component_t camera;
  component_t encoder;
  component_t render;
  camera.name = "OMX.broadcom.camera";
  encoder.name = "OMX.broadcom.video_encode";
  render.name = "OMX.broadcom.video_render";

  signal(SIGINT, intHandler);
  
  printf ("start\n");

  //Open the file
  int fd = open (FILENAME, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0666);
  if (fd == -1){
    fprintf (stderr, "error: open\n");
    exit (1);
  }
  
  //Initialize Broadcom's VideoCore APIs
  bcm_host_init ();
  
  //Initialize OpenMAX IL
  if ((error = OMX_Init ())){
    fprintf (stderr, "error: OMX_Init: %s\n", dump_OMX_ERRORTYPE (error));
    exit (1);
  }


  
  //Initialize components
  init_component (&camera);
  init_component (&encoder);
  init_component (&render);
  
  //Initialize camera drivers
  load_camera_drivers (&camera);
  
  //Configure camera port definition
  printf ("configuring %s port definition\n", camera.name);
  OMX_PARAM_PORTDEFINITIONTYPE port_st;
  OMX_INIT_STRUCTURE (port_st);
  port_st.nPortIndex = 71;
  if ((error = OMX_GetParameter (camera.handle, OMX_IndexParamPortDefinition,
      &port_st))){
    fprintf (stderr, "error: OMX_GetParameter: %s\n",
        dump_OMX_ERRORTYPE (error));
    exit (1);
  }
  
  port_st.format.video.nFrameWidth = CAM_WIDTH;
  port_st.format.video.nFrameHeight = CAM_HEIGHT;
  port_st.format.video.nStride = CAM_WIDTH;
  port_st.format.video.xFramerate = VIDEO_FRAMERATE << 16;
  port_st.format.video.eCompressionFormat = OMX_VIDEO_CodingUnused;
  port_st.format.video.eColorFormat = OMX_COLOR_FormatYUV420PackedPlanar;
  if ((error = OMX_SetParameter (camera.handle, OMX_IndexParamPortDefinition,
      &port_st))){
    fprintf (stderr, "error: OMX_SetParameter: %s\n",
        dump_OMX_ERRORTYPE (error));
    exit (1);
  }
  
  //Preview port
  port_st.nPortIndex = 70;
  if ((error = OMX_SetParameter (camera.handle, OMX_IndexParamPortDefinition,
      &port_st))){
    fprintf (stderr, "error: OMX_SetParameter: %s\n",
        dump_OMX_ERRORTYPE (error));
    exit (1);
  }
  
  printf ("configuring %s framerate\n", camera.name);
  OMX_CONFIG_FRAMERATETYPE framerate_st;
  OMX_INIT_STRUCTURE (framerate_st);
  framerate_st.nPortIndex = 71;
  framerate_st.xEncodeFramerate = port_st.format.video.xFramerate;
  if ((error = OMX_SetConfig (camera.handle, OMX_IndexConfigVideoFramerate,
      &framerate_st))){
    fprintf (stderr, "error: OMX_SetConfig: %s\n", dump_OMX_ERRORTYPE (error));
    exit (1);
  }
  
  //Preview port
  framerate_st.nPortIndex = 70;
  if ((error = OMX_SetConfig (camera.handle, OMX_IndexConfigVideoFramerate,
      &framerate_st))){
    fprintf (stderr, "error: OMX_SetConfig: %s\n", dump_OMX_ERRORTYPE (error));
    exit (1);
  }
  
  //Configure camera settings
  set_camera_settings (&camera);
  
  //Configure encoder port definition
  printf ("configuring %s port definition\n", encoder.name);
  OMX_INIT_STRUCTURE (port_st);
  port_st.nPortIndex = 201;
  if ((error = OMX_GetParameter (encoder.handle, OMX_IndexParamPortDefinition,
      &port_st))){
    fprintf (stderr, "error: OMX_GetParameter: %s\n",
        dump_OMX_ERRORTYPE (error));
    exit (1);
  }
  port_st.format.video.nFrameWidth = CAM_WIDTH;
  port_st.format.video.nFrameHeight = CAM_HEIGHT;
  port_st.format.video.nStride = CAM_WIDTH;
  port_st.format.video.xFramerate = VIDEO_FRAMERATE << 16;
  //Despite being configured later, these two fields need to be set
  port_st.format.video.nBitrate = VIDEO_QP ? 0 : VIDEO_BITRATE;
  port_st.format.video.eCompressionFormat = OMX_VIDEO_CodingAVC;
  if ((error = OMX_SetParameter (encoder.handle, OMX_IndexParamPortDefinition,
      &port_st))){
    fprintf (stderr, "error: OMX_SetParameter: %s\n",
        dump_OMX_ERRORTYPE (error));
    exit (1);
  }

  //Configure render port definition
  printf ("configuring %s port definition\n", render.name);
  OMX_INIT_STRUCTURE (port_st);
  // HS: not sure this is required, the encode only configures the
  // output port
  port_st.nPortIndex = 90;
  if ((error = OMX_GetParameter (render.handle, OMX_IndexParamPortDefinition,
      &port_st))){
    fprintf (stderr, "error: OMX_GetParameter: %s\n",
        dump_OMX_ERRORTYPE (error));
    exit (1);
  }

  printf ("config %s input buffer with size %i %i (%ix%i)\n", render.name, port_st.nBufferSize, port_st.eDomain, port_st.format.video.nFrameWidth, port_st.format.video.nFrameHeight);

#ifdef HS_INTERCEPT
  // disabled this block seems it is not required, instead the header
  // which needs to be sent before the data probably holds this
  // information, see
  // file:///usr/share/doc/libraspberrypi-doc/ilcomponents/video_encode.html.
  port_st.format.video.nFrameWidth = 1920;
  port_st.format.video.nFrameHeight = 1080;
  port_st.format.video.nSliceHeight = 1088;
  port_st.format.video.nStride = 1920;
  port_st.format.video.xFramerate = VIDEO_FRAMERATE << 16;
  port_st.format.video.pNativeRender = 0;
  //port_st.format.video.xFramerate = VIDEO_FRAMERATE << 16;
  /* //Despite being configured later, these two fields need to be set */
  port_st.format.video.eColorFormat = OMX_COLOR_FormatYUV420PackedPlanar;
  //  port_st.format.video.eColorFormat = OMX_COLOR_Format32bitARGB8888;
  port_st.nBufferSize = -1;
  if ((error = OMX_SetParameter (render.handle, OMX_IndexParamPortDefinition,
      &port_st))){
    fprintf (stderr, "error: OMX_SetParameter: %s\n",
        dump_OMX_ERRORTYPE (error));
    exit (1);
  }
#endif

  //Configure H264
  set_h264_settings (&encoder);
  
  //Setup tunnels: camera (video) -> video_encode, camera (preview) -> video_render
  printf ("configuring tunnels\n");
  if ((error = OMX_SetupTunnel (camera.handle, 71, encoder.handle, 200))){
    fprintf (stderr, "error: OMX_SetupTunnel: %s\n",
        dump_OMX_ERRORTYPE (error));
    exit (1);
  }
#ifndef HS_INTERCEPT
  if ((error = OMX_SetupTunnel (camera.handle, 70, render.handle, 90))){
    fprintf (stderr, "error: OMX_SetupTunnel: %s\n",
        dump_OMX_ERRORTYPE (error));
    exit (1);
  }
#endif
  
  //Change state to IDLE
  change_state (&camera, OMX_StateIdle);
  wait (&camera, EVENT_STATE_SET, 0);
  change_state (&encoder, OMX_StateIdle);
  wait (&encoder, EVENT_STATE_SET, 0);
  change_state (&render, OMX_StateIdle);
  wait (&render, EVENT_STATE_SET, 0);
   
#ifdef HS_INTERCEPT
  set_render_config(&render);
#endif
  
  //Enable the ports
#ifdef HS_RECORD
  enable_port (&camera, 71);
  wait (&camera, EVENT_PORT_ENABLE, 0);
#endif
  enable_port (&camera, 70);
#ifdef HS_INTERCEPT
  enable_camera_output_port (&camera, &camera_output_buffer);
#endif
  wait (&camera, EVENT_PORT_ENABLE, 0);
  enable_port (&render, 90);
#ifdef HS_INTERCEPT
  enable_render_input_port (&render, render_input_buffer);
#endif
  printf ("called UseBuffer\n");
  wait (&render, EVENT_PORT_ENABLE, 0);
  printf ("wait ended\n");
  enable_port (&encoder, 200);
  wait (&encoder, EVENT_PORT_ENABLE, 0);
  enable_port (&encoder, 201);
  enable_encoder_output_port (&encoder, &encoder_output_buffer);
  wait (&encoder, EVENT_PORT_ENABLE, 0);

  dbg_print_render(&render);

  //Change state to EXECUTING
  change_state (&camera, OMX_StateExecuting);
  wait (&camera, EVENT_STATE_SET, 0);
  change_state (&encoder, OMX_StateExecuting);
  wait (&encoder, EVENT_STATE_SET, 0);
  wait (&encoder, EVENT_PORT_SETTINGS_CHANGED, 0);
  change_state (&render, OMX_StateExecuting);
  wait (&render, EVENT_STATE_SET, 0);
  
  dbg_print_render(&render);

  //Enable camera capture port. This basically says that the port 71 will be
  //used to get data from the camera. If you're capturing a still, the port 72
  //must be used
  printf ("enabling %s capture port\n", camera.name);
  OMX_CONFIG_PORTBOOLEANTYPE capture_st;
  OMX_INIT_STRUCTURE (capture_st);
  capture_st.nPortIndex = 71;
  capture_st.bEnabled = OMX_TRUE;
  if ((error = OMX_SetConfig (camera.handle, OMX_IndexConfigPortCapturing,
      &capture_st))){
    fprintf (stderr, "error: OMX_SetConfig: %s\n", dump_OMX_ERRORTYPE (error));
    exit (1);
  }
  
  //Record ~3000 ms
  struct timespec spec;
  clock_gettime (CLOCK_MONOTONIC, &spec);
  long now = spec.tv_sec*1000 + spec.tv_nsec/1.0e6;
  long end = now + 30000;

#ifdef HS_INTERCEPT
  int renderbuf=-1;
  bool firstUse = 1;
  while (sKeepRunning){
    int offset = 0;
    //    offset<1920*1088*3/2
    renderbuf = (renderbuf + 1) % 3;

    VCOS_UNSIGNED retrieved_events;
    if (!firstUse)
    {
      // I should first make sure the buffer is empty, unfortunately this blocks
      wait (&render, EVENT_EMPTY_BUFFER_DONE, &retrieved_events);
      fprintf(stderr, "here we are the buffer is empty = %i\n", retrieved_events);
    }
    if (2==renderbuf) firstUse=false;

    printf ("camera_output_buffer->nFlags = %i\n", camera_output_buffer->nFlags);
    while (!(OMX_BUFFERFLAG_ENDOFFRAME & (camera_output_buffer->nFlags)))
      {
	printf ("camera_output_buffer->nFlags = %i\n", camera_output_buffer->nFlags);
	//Get the buffer data
	if ((error = OMX_FillThisBuffer (camera.handle, camera_output_buffer))){
	  fprintf (stderr, "error: OMX_FillThisBuffer: %s\n",
		   dump_OMX_ERRORTYPE (error));
	  exit (1);
	}
    
        //usleep(100);
	printf ("now let's wait until the buffer is filled\n");
	//Wait until it's filled
	wait (&camera, EVENT_FILL_BUFFER_DONE, 0);

	printf ("here we are we got the image %i %i 0x%x %x %x %x %x\n",
		camera_output_buffer->nFilledLen,
		camera_output_buffer->nOffset,
		camera_output_buffer->nFlags,
		*(unsigned int*)(camera_output_buffer->pBuffer),
		*(unsigned int*)(camera_output_buffer->pBuffer+4),
		*(unsigned int*)(camera_output_buffer->pBuffer+8),
		*(unsigned int*)(camera_output_buffer->pBuffer+12));

	
	memcpy(render_input_buffer[renderbuf]->pBuffer+offset, camera_output_buffer->pBuffer,
	       camera_output_buffer->nFilledLen*2/3);
	memcpy(render_input_buffer[renderbuf]->pBuffer+1920*1088+offset/4,
	       camera_output_buffer->pBuffer + camera_output_buffer->nFilledLen*2/3,
	       camera_output_buffer->nFilledLen/6);
	memcpy(render_input_buffer[renderbuf]->pBuffer+1920*1088*5/4+offset/4,
	       camera_output_buffer->pBuffer + camera_output_buffer->nFilledLen*5/6,
	       camera_output_buffer->nFilledLen/6);
	offset += camera_output_buffer->nFilledLen*2/3;
      }
    camera_output_buffer->nFlags = 0;
    render_input_buffer[renderbuf]->nFilledLen = offset*3/2;
    render_input_buffer[renderbuf]->nFlags = OMX_BUFFERFLAG_ENDOFFRAME | OMX_BUFFERFLAG_SYNCFRAME | OMX_BUFFERFLAG_EOS;
    render_input_buffer[renderbuf]->nOffset = 0;
    printf ("end of frame got %i / %i\n",
	    render_input_buffer[renderbuf]->nFilledLen,
	    render_input_buffer[renderbuf]->nAllocLen);

    if ((error = OMX_EmptyThisBuffer(render.handle, render_input_buffer[renderbuf]))){
      fprintf (stderr, "error: OMX_EmptyThisBuffer: %s\n",
          dump_OMX_ERRORTYPE (error));
      exit (1);
    }

#ifdef HS_RECORD
    encoder_output_buffer->nFlags = 0;
    while (!(OMX_BUFFERFLAG_ENDOFFRAME & (encoder_output_buffer->nFlags))) {
      //Get the buffer data
      if ((error = OMX_FillThisBuffer
	   (encoder.handle, encoder_output_buffer))){
	fprintf (stderr, "error: OMX_FillThisBuffer: %s\n",
		 dump_OMX_ERRORTYPE (error));
	exit (1);
      }

      printf ("here does it now continue?\n");
      //Wait until it's filled
      wait (&encoder, EVENT_FILL_BUFFER_DONE, 0);
   
      printf ("here we are we got the encoded %i %i 0x%x %x %x %x %x\n",
	      encoder_output_buffer->nFilledLen,
	      encoder_output_buffer->nOffset,
	      encoder_output_buffer->nFlags,
	      *(unsigned int*)(encoder_output_buffer->pBuffer),
	      *(unsigned int*)(encoder_output_buffer->pBuffer+4),
	      *(unsigned int*)(encoder_output_buffer->pBuffer+8),
	      *(unsigned int*)(encoder_output_buffer->pBuffer+12));

      //Append the buffer into the file
      if (pwrite (fd, encoder_output_buffer->pBuffer,
		encoder_output_buffer->nFilledLen,
		  encoder_output_buffer->nOffset) == -1){
	fprintf (stderr, "error: pwrite\n");
	exit (1);
      }
    }
#endif
    
    clock_gettime (CLOCK_MONOTONIC, &spec);
    printf ("running for %g\n", spec.tv_sec + spec.tv_nsec/1.0e9- end/1000.);
    //    if (spec.tv_sec*1000 + spec.tv_nsec/1.0e6 >= end) break;
  }
#else
  while (sKeepRunning){
    //Get the buffer data
    if ((error = OMX_FillThisBuffer (encoder.handle, encoder_output_buffer))){
      fprintf (stderr, "error: OMX_FillThisBuffer: %s\n",
          dump_OMX_ERRORTYPE (error));
      exit (1);
    }
    
    //Wait until it's filled
    wait (&encoder, EVENT_FILL_BUFFER_DONE, 0);
    
    //Append the buffer into the file
    if (pwrite (fd, encoder_output_buffer->pBuffer,
        encoder_output_buffer->nFilledLen,
        encoder_output_buffer->nOffset) == -1){
      fprintf (stderr, "error: pwrite\n");
      exit (1);
    }
    
    clock_gettime (CLOCK_MONOTONIC, &spec);
    //    if (spec.tv_sec*1000 + spec.tv_nsec/1.0e6 >= end) break;
  }
#endif
  
  dbg_print_render(&render);

  printf ("------------------------------------------------\n");
  
  //Disable camera capture port
  printf ("disabling %s capture port\n", camera.name);
  capture_st.bEnabled = OMX_FALSE;
  if ((error = OMX_SetConfig (camera.handle, OMX_IndexConfigPortCapturing,
      &capture_st))){
    fprintf (stderr, "error: OMX_SetConfig: %s\n", dump_OMX_ERRORTYPE (error));
    exit (1);
  }
  
  //Change state to IDLE
  change_state (&camera, OMX_StateIdle);
  wait (&camera, EVENT_STATE_SET, 0);
  change_state (&encoder, OMX_StateIdle);
  wait (&encoder, EVENT_STATE_SET, 0);
  change_state (&render, OMX_StateIdle);
  wait (&render, EVENT_STATE_SET, 0);
  
  //Disable the tunnel ports
#ifdef HS_RECORD
  disable_port (&camera, 71);
  wait (&camera, EVENT_PORT_DISABLE, 0);
#endif
  disable_port (&camera, 70);
#ifdef HS_INTERCEPT
  disable_camera_output_port (&camera, camera_output_buffer);
#endif
  wait (&camera, EVENT_PORT_DISABLE, 0);
  disable_port (&render, 90);
#ifdef HS_INTERCEPT
  disable_render_input_port (&render, render_input_buffer);
#endif
  wait (&render, EVENT_PORT_DISABLE, 0);
  disable_port (&encoder, 200);
  wait (&encoder, EVENT_PORT_DISABLE, 0);
  disable_port (&encoder, 201);
  disable_encoder_output_port (&encoder, encoder_output_buffer);
  wait (&encoder, EVENT_PORT_DISABLE, 0);
  
  //Change state to LOADED
  change_state (&camera, OMX_StateLoaded);
  wait (&camera, EVENT_STATE_SET, 0);
  change_state (&encoder, OMX_StateLoaded);
  wait (&encoder, EVENT_STATE_SET, 0);
  change_state (&render, OMX_StateLoaded);
  wait (&render, EVENT_STATE_SET, 0);
  
  //Deinitialize components
  deinit_component (&camera);
  deinit_component (&encoder);
  deinit_component (&render);
  
  //Deinitialize OpenMAX IL
  if ((error = OMX_Deinit ())){
    fprintf (stderr, "error: OMX_Deinit: %s\n", dump_OMX_ERRORTYPE (error));
    exit (1);
  }
  
  //Deinitialize Broadcom's VideoCore APIs
  bcm_host_deinit ();
  
  //Close the file
  if (close (fd)){
    fprintf (stderr, "error: close\n");
    exit (1);
  }
  
  printf ("ok\n");
  
  return 0;
}
