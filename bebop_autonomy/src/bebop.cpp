#include <stdexcept>
#include <cmath>
#include <string>

#include <boost/bind.hpp>
#include <boost/thread/locks.hpp>

extern "C"
{
  #include "libARCommands/ARCommands.h"
}

#include <bebop_autonomy/bebop.h>

namespace bebop_autonomy
{

const char* Bebop::LOG_TAG = "BebopSDK";

void Bebop::BatteryStateChangedCallback (uint8_t percent, void *bebop_void_ptr)
{
  ARSAL_PRINT(ARSAL_PRINT_WARNING, LOG_TAG, "bat: %d", percent);
}

void Bebop::StateChangedCallback(eARCONTROLLER_DEVICE_STATE new_state, eARCONTROLLER_ERROR error, void *bebop_void_ptr)
{
  // TODO: Log error
  Bebop* bebop_ptr_ = static_cast<Bebop*>(bebop_void_ptr);

  switch (new_state)
  {
  case ARCONTROLLER_DEVICE_STATE_STOPPED:
    ARSAL_Sem_Post(&(bebop_ptr_->state_sem_));
    break;
  case ARCONTROLLER_DEVICE_STATE_RUNNING:
    ARSAL_Sem_Post(&(bebop_ptr_->state_sem_));
    break;
  }
}

void Bebop::CommandReceivedCallback(eARCONTROLLER_DICTIONARY_KEY cmd_key, ARCONTROLLER_DICTIONARY_ELEMENT_t *element_dict_ptr, void *bebop_void_ptr)
{
  static long int lwp_id = util::GetLWPId();
  static bool lwp_id_printed = false;
  if (!lwp_id_printed)
  {
    ARSAL_PRINT(ARSAL_PRINT_INFO, LOG_TAG, "Command Received Callback LWP id is: %ld", lwp_id);
    lwp_id_printed = true;
  }
  Bebop* bebop_ptr_ = static_cast<Bebop*>(bebop_void_ptr);

  ARCONTROLLER_DICTIONARY_ELEMENT_t *single_element_ptr = NULL;

  if (element_dict_ptr)
  {
    // We are only interested in single key dictionaries
    HASH_FIND_STR (element_dict_ptr, ARCONTROLLER_DICTIONARY_SINGLE_KEY, single_element_ptr);

    if (single_element_ptr)
    {
      std::map<eARCONTROLLER_DICTIONARY_KEY, boost::shared_ptr<cb::CommandBase> >::iterator it = bebop_ptr_->callback_map_.find(cmd_key);
      if (it != bebop_ptr_->callback_map_.end())
      {
        // TODO: Check if we can find the time from the packets
        it->second->Update(element_dict_ptr->arguments, ros::Time::now());
      }
    }
  }
}

// This Callback runs in ARCONTROLLER_Stream_ReaderThreadRun context and blocks it until it returns
void Bebop::FrameReceivedCallback(ARCONTROLLER_Frame_t *frame, void *bebop_void_ptr_)
{
  static long int lwp_id = util::GetLWPId();
  static bool lwp_id_printed = false;
  if (!lwp_id_printed)
  {
    ARSAL_PRINT(ARSAL_PRINT_INFO, LOG_TAG, "Frame Recv & Decode LWP id: %ld", lwp_id);
    lwp_id_printed = true;
  }

  if (!frame)
  {
    ARSAL_PRINT(ARSAL_PRINT_WARNING, LOG_TAG, "Received frame is NULL");
    return;
  }

  Bebop* bebop_ptr = static_cast<Bebop*>(bebop_void_ptr_);
  // TODO: FixMe
  frame->width = 640;
  frame->height = 368;

  {
    boost::unique_lock<boost::mutex> lock(bebop_ptr->frame_avail_mutex_);
    if (bebop_ptr->is_frame_avail_)
    {
      ARSAL_PRINT(ARSAL_PRINT_WARNING, LOG_TAG, "Previous frame might have been missed.");
    }

    if (!bebop_ptr->video_decoder_.Decode(frame))
    {
      ARSAL_PRINT(ARSAL_PRINT_ERROR, LOG_TAG, "Video decode failed");
    }
    else
    {
      bebop_ptr->is_frame_avail_ = true;
      bebop_ptr->frame_avail_cond_.notify_one();
    }
  }
}


Bebop::Bebop(ARSAL_Print_Callback_t custom_print_callback):
  is_connected_(false),
  device_ptr_(NULL),
  device_controller_ptr_(NULL),
  error_(ARCONTROLLER_OK),
  device_state_(ARCONTROLLER_DEVICE_STATE_MAX),
  video_decoder_(),
  is_frame_avail_(false)
//  out_file("/tmp/ts.txt")
{
  // Redirect all calls to AR_PRINT_* to this function if provided
  if (custom_print_callback)
    ARSAL_Print_SetCallback(custom_print_callback);

  ARSAL_PRINT(ARSAL_PRINT_INFO, LOG_TAG, "Bebop Cnstr()");
}

Bebop::~Bebop()
{
//  out_file.close();
  // This is the last resort, the program must run Cleanup() fo
  // proper disconnection and free
  if (device_ptr_) ARDISCOVERY_Device_Delete(&device_ptr_);
  if (device_controller_ptr_) ARCONTROLLER_Device_Delete(&device_controller_ptr_);
}

void Bebop::Connect(ros::NodeHandle& nh, ros::NodeHandle& priv_nh)
{
  try
  {
    if (is_connected_) throw std::runtime_error("Already inited");

    // TODO: Error checking;
    ARSAL_Sem_Init(&state_sem_, 0, 0);

    eARDISCOVERY_ERROR error_discovery = ARDISCOVERY_OK;
    device_ptr_ = ARDISCOVERY_Device_New(&error_discovery);

    if (error_discovery != ARDISCOVERY_OK)
    {
      throw std::runtime_error("Discovery failed: " + std::string(ARDISCOVERY_Error_ToString(error_discovery)));
    }

    error_discovery = ARDISCOVERY_Device_InitWifi(device_ptr_,
                                                  ARDISCOVERY_PRODUCT_ARDRONE, "Bebop",
                                                  "192.168.42.1", 44444);

    if (error_discovery != ARDISCOVERY_OK)
    {
      throw std::runtime_error("Discovery failed: " + std::string(ARDISCOVERY_Error_ToString(error_discovery)));
    }

    device_controller_ptr_ = ARCONTROLLER_Device_New(device_ptr_, &error_);
    ThrowOnCtrlError(error_, "Creation of device controller failed: ");

    ARDISCOVERY_Device_Delete(&device_ptr_);

    ThrowOnCtrlError(
          ARCONTROLLER_Device_AddStateChangedCallback(device_controller_ptr_, Bebop::StateChangedCallback, (void*) this),
          "Registering state callback failed");
    ThrowOnCtrlError(
          ARCONTROLLER_Device_AddCommandReceivedCallback(device_controller_ptr_, Bebop::CommandReceivedCallback, (void*) this),
          "Registering command callback failed");
    // third argument is frame timeout callback
    ThrowOnCtrlError(
          ARCONTROLLER_Device_SetVideoReceiveCallback (device_controller_ptr_, Bebop::FrameReceivedCallback, NULL , (void*) this),
          "Registering video callback failed");


    ThrowOnCtrlError(ARCONTROLLER_Device_Start(device_controller_ptr_), "Controller device start failed");

    // Semaphore is touched inside the StateCallback
    ARSAL_Sem_Wait(&state_sem_);

    device_state_ = ARCONTROLLER_Device_GetState(device_controller_ptr_, &error_);
    if ((error_ != ARCONTROLLER_OK) || (device_state_ != ARCONTROLLER_DEVICE_STATE_RUNNING))
    {
      throw std::runtime_error("Waiting for device failed: " + std::string(ARCONTROLLER_Error_ToString(error_)));
    }

    // Start video streaming
    ThrowOnCtrlError(device_controller_ptr_->aRDrone3->sendMediaStreamingVideoEnable(
                       device_controller_ptr_->aRDrone3, 1), "Starting video stream failed.");

#define BEBOP_UPDTAE_CALLBACK_MAP
#include "bebop_autonomy/autogenerated/bebop_common_callback_includes.h"
#include "bebop_autonomy/autogenerated/bebop_ardrone3_callback_includes.h"
#undef BEBOP_UPDTAE_CALLBACK_MAP

  }
  catch (const std::runtime_error& e)
  {
    Cleanup();
    throw e;
  }

  is_connected_ = true;
  ARSAL_PRINT(ARSAL_PRINT_INFO, LOG_TAG, "BebopSDK inited, lwp_id: %ld", util::GetLWPId());
}

void Bebop::Cleanup()
{
  ARSAL_PRINT(ARSAL_PRINT_INFO, LOG_TAG, "Bebop Cleanup()");
  if (device_controller_ptr_)
  {
    device_state_ = ARCONTROLLER_Device_GetState(device_controller_ptr_, &error_);
    if ((error_ == ARCONTROLLER_OK) && (device_state_ != ARCONTROLLER_DEVICE_STATE_STOPPED))
    {
      // Say disconnecting
      error_ = ARCONTROLLER_Device_Stop(device_controller_ptr_);
      if (error_ == ARCONTROLLER_OK)
      {
        ARSAL_Sem_Wait(&state_sem_);
      }
    }
    ARCONTROLLER_Device_Delete(&device_controller_ptr_);
  }
  ARSAL_Sem_Destroy(&state_sem_);
}

bool Bebop::Disconnect()
{
  if (!is_connected_) return false;
  Cleanup();
  ARSAL_PRINT(ARSAL_PRINT_INFO, LOG_TAG, "-- END --");
  return true;
}

void Bebop::Takeoff()
{
  ThrowOnInternalError("Takeoff failed");
  ThrowOnCtrlError(
        device_controller_ptr_->aRDrone3->sendPilotingTakeOff(device_controller_ptr_->aRDrone3),
        "Takeoff failed");
}

void Bebop::Land()
{
  ThrowOnInternalError("Land failed");
  ThrowOnCtrlError(
        device_controller_ptr_->aRDrone3->sendPilotingLanding(device_controller_ptr_->aRDrone3),
        "Land failed");
}

void Bebop::Move(const double &roll, const double &pitch, const double &gaz_speed, const double &yaw_speed)
{
  // TODO: Bound check
  ThrowOnInternalError("Move failure");

  // If roll or pitch value are non-zero, enabel roll/pitch flag
  const bool do_rp = !((fabs(roll) < 0.001) && (fabs(pitch) < 0.001));

  // If all values are zero, hover
  const bool do_hover = !do_rp && (fabs(yaw_speed) < 0.001) && (fabs(gaz_speed) < 0.001);

  if (do_hover)
  {
    ARSAL_PRINT(ARSAL_PRINT_ERROR, LOG_TAG, "STOP");
    ThrowOnCtrlError(
          device_controller_ptr_->aRDrone3->setPilotingPCMD(
            device_controller_ptr_->aRDrone3,
            0, 0, 0, 0, 0, 0));
  }
  else
  {
    ThrowOnCtrlError(
          device_controller_ptr_->aRDrone3->setPilotingPCMD(
            device_controller_ptr_->aRDrone3,
            do_rp,
            roll * 100.0,
            pitch * 100.0,
            yaw_speed * 100.0,
            gaz_speed * 100.0,
            0));
  }
}

// in degrees
void Bebop::MoveCamera(const double &tilt, const double &pan)
{
  ThrowOnInternalError("Camera Move Failure");
  ThrowOnCtrlError(device_controller_ptr_->aRDrone3->sendCameraOrientation(
                     device_controller_ptr_->aRDrone3,
                     static_cast<int8_t>(tilt),
                     static_cast<int8_t>(pan)));
}

bool Bebop::GetFrontCameraFrame(std::vector<uint8_t> &buffer, uint32_t& width, uint32_t& height) const
{
  boost::unique_lock<boost::mutex> lock(frame_avail_mutex_);

  ARSAL_PRINT(ARSAL_PRINT_DEBUG, LOG_TAG, "Waiting for frame to become available ...");
//  ARSAL_PRINT(ARSAL_PRINT_WARNING, LOG_TAG, "By the way, roll is: %f", attitude_changed_ptr_->GetDataCstPtr()->data);
  while (!is_frame_avail_)
  {
    frame_avail_cond_.wait(lock);
  }

  const uint32_t num_bytes = video_decoder_.GetFrameWidth() * video_decoder_.GetFrameHeight() * 3;

  buffer.resize(num_bytes);
  // New frame is ready
  std::copy(video_decoder_.GetFrameRGBRawCstPtr(),
            video_decoder_.GetFrameRGBRawCstPtr() + num_bytes,
            buffer.begin());

  width = video_decoder_.GetFrameWidth();
  height = video_decoder_.GetFrameHeight();
  is_frame_avail_ = false;
  return true;
}

void Bebop::ThrowOnInternalError(const std::string &message)
{
  if (!is_connected_ || !device_controller_ptr_)
  {
    throw std::runtime_error(message);
  }
}

void Bebop::ThrowOnCtrlError(const eARCONTROLLER_ERROR &error, const std::string &message)
{
  if (error != ARCONTROLLER_OK)
  {
    throw std::runtime_error(message + std::string(ARCONTROLLER_Error_ToString(error)));
  }
}

}  // namespace bebop_autonomys