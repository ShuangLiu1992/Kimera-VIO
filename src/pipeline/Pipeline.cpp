/* ----------------------------------------------------------------------------
 * Copyright 2017, Massachusetts Institute of Technology,
 * Cambridge, MA 02139
 * All Rights Reserved
 * Authors: Luca Carlone, et al. (see THANKS for the full author list)
 * See LICENSE for the license information
 * -------------------------------------------------------------------------- */

/**
 * @file   Pipeline.cpp
 * @brief  Implements VIO pipeline workflow.
 * @author Antoni Rosinol
 */

#include "kimera-vio/pipeline/Pipeline.h"

#include <future>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <gflags/gflags.h>
#include <glog/logging.h>

#include <gtsam/geometry/Pose3.h>

#include "kimera-vio/backend/VioBackEndFactory.h"
#include "kimera-vio/frontend/VisionFrontEndFactory.h"
#include "kimera-vio/mesh/MesherFactory.h"
#include "kimera-vio/utils/Statistics.h"
#include "kimera-vio/utils/Timer.h"
#include "kimera-vio/visualizer/DisplayFactory.h"
#include "kimera-vio/visualizer/Visualizer3D.h"
#include "kimera-vio/visualizer/Visualizer3DFactory.h"

DEFINE_bool(log_output, false, "Log output to CSV files.");
DEFINE_bool(extract_planes_from_the_scene,
            false,
            "Whether to use structural regularities in the scene,"
            "currently only planes.");

DEFINE_bool(visualize, true, "Enable overall visualization.");
DEFINE_bool(visualize_lmk_type, false, "Enable landmark type visualization.");
DEFINE_int32(viz_type,
             0,
             "0: MESH2DTo3Dsparse, get a 3D mesh from a 2D triangulation of "
             "the (right-VALID).\n"
             "1: POINTCLOUD, visualize 3D VIO points (no repeated point)\n"
             "are re-plotted at every frame).\n"
             "keypoints in the left frame and filters out triangles \n"
             "2: NONE, does not visualize map.");

DEFINE_bool(deterministic_random_number_generator,
            false,
            "If true the random number generator will consistently output the "
            "same sequence of pseudo-random numbers for every run (use it to "
            "have repeatable output). If false the random number generator "
            "will output a different sequence for each run.");
DEFINE_int32(min_num_obs_for_mesher_points,
             4,
             "Minimum number of observations for a smart factor's landmark to "
             "to be used as a 3d point to consider for the mesher.");

DEFINE_bool(use_lcd,
            false,
            "Enable LoopClosureDetector processing in pipeline.");

namespace VIO {

Pipeline::Pipeline(const VioParams& params,
                   Visualizer3D::UniquePtr&& visualizer,
                   DisplayBase::UniquePtr&& displayer)
    : backend_params_(params.backend_params_),
      frontend_params_(params.frontend_params_),
      imu_params_(params.imu_params_),
      backend_type_(static_cast<BackendType>(params.backend_type_)),
      parallel_run_(params.parallel_run_),
      stereo_camera_(nullptr),
      data_provider_module_(nullptr),
      vio_frontend_module_(nullptr),
      stereo_frontend_input_queue_("stereo_frontend_input_queue"),
      vio_backend_module_(nullptr),
      backend_input_queue_("backend_input_queue"),
      mesher_module_(nullptr),
      lcd_module_(nullptr),
      visualizer_module_(nullptr),
      display_input_queue_("display_input_queue"),
      display_module_(nullptr),
      shutdown_pipeline_cb_(nullptr),
      frontend_thread_(nullptr),
      backend_thread_(nullptr),
      mesher_thread_(nullptr),
      lcd_thread_(nullptr),
      visualizer_thread_(nullptr) {
  if (FLAGS_deterministic_random_number_generator) {
    setDeterministicPipeline();
  }

  //! Create Stereo Camera
  CHECK_EQ(params.camera_params_.size(), 2u) << "Only stereo camera support.";
  stereo_camera_ = VIO::make_unique<StereoCamera>(
      params.camera_params_.at(0),
      params.camera_params_.at(1),
      params.frontend_params_.stereo_matching_params_);

  //! Create DataProvider
  data_provider_module_ = VIO::make_unique<DataProviderModule>(
      &stereo_frontend_input_queue_,
      "Data Provider",
      parallel_run_,
      // TODO(Toni): these params should not be sent...
      params.frontend_params_.stereo_matching_params_);

  data_provider_module_->registerVioPipelineCallback(
      std::bind(&Pipeline::spinOnce, this, std::placeholders::_1));

  //! Create frontend
  vio_frontend_module_ = VIO::make_unique<StereoVisionFrontEndModule>(
      &stereo_frontend_input_queue_,
      parallel_run_,
      VisionFrontEndFactory::createFrontend(
          params.frontend_type_,
          params.imu_params_,
          gtsam::imuBias::ConstantBias(),
          params.frontend_params_,
          params.camera_params_.at(0),
          FLAGS_visualize ? &display_input_queue_ : nullptr,
          FLAGS_log_output));
  auto& backend_input_queue = backend_input_queue_;  //! for the lambda below
  vio_frontend_module_->registerOutputCallback(
      [&backend_input_queue](const FrontendOutput::Ptr& output) {
        CHECK(output);
        if (output->is_keyframe_) {
          //! Only push to backend input queue if it is a keyframe!
          backend_input_queue.push(VIO::make_unique<BackendInput>(
              output->stereo_frame_lkf_.getTimestamp(),
              output->status_stereo_measurements_,
              output->tracker_status_,
              output->pim_,
              output->imu_acc_gyrs_,
              output->relative_pose_body_stereo_));
        } else {
          VLOG(5)
              << "Frontend did not output a keyframe, skipping backend input.";
        }
      });

  //! Params for what the backend outputs.
  // TODO(Toni): put this into backend params.
  BackendOutputParams backend_output_params(
      static_cast<VisualizationType>(FLAGS_viz_type) !=
          VisualizationType::kNone,
      FLAGS_min_num_obs_for_mesher_points,
      FLAGS_visualize && FLAGS_visualize_lmk_type);

  //! Create backend
  CHECK(backend_params_);
  vio_backend_module_ = VIO::make_unique<VioBackEndModule>(
      &backend_input_queue_,
      parallel_run_,
      BackEndFactory::createBackend(backend_type_,
                                    // These two should be given by parameters.
                                    stereo_camera_->getLeftCamRectPose(),
                                    stereo_camera_->getStereoCalib(),
                                    *backend_params_,
                                    imu_params_,
                                    backend_output_params,
                                    FLAGS_log_output));
  vio_backend_module_->registerOnFailureCallback(
      std::bind(&Pipeline::signalBackendFailure, this));
  vio_backend_module_->registerImuBiasUpdateCallback(
      std::bind(&StereoVisionFrontEndModule::updateImuBias,
                // Send a cref: constant reference bcs updateImuBias is const
                std::cref(*CHECK_NOTNULL(vio_frontend_module_.get())),
                std::placeholders::_1));

  if (static_cast<VisualizationType>(FLAGS_viz_type) ==
      VisualizationType::kMesh2dTo3dSparse) {
    mesher_module_ = VIO::make_unique<MesherModule>(
        parallel_run_,
        MesherFactory::createMesher(
            MesherType::PROJECTIVE,
            MesherParams(stereo_camera_->getLeftCamRectPose(),
                         params.camera_params_.at(0u).image_size_)));
    //! Register input callbacks
    vio_backend_module_->registerOutputCallback(
        std::bind(&MesherModule::fillBackendQueue,
                  std::ref(*CHECK_NOTNULL(mesher_module_.get())),
                  std::placeholders::_1));
    vio_frontend_module_->registerOutputCallback(
        std::bind(&MesherModule::fillFrontendQueue,
                  std::ref(*CHECK_NOTNULL(mesher_module_.get())),
                  std::placeholders::_1));
  }

  if (FLAGS_visualize) {
    visualizer_module_ = VIO::make_unique<VisualizerModule>(
        //! Send ouput of visualizer to the display_input_queue_
        &display_input_queue_,
        parallel_run_,
        // Use given visualizer if any
        visualizer ? std::move(visualizer)
                   : VisualizerFactory::createVisualizer(
                         VisualizerType::OpenCV,
                         // TODO(Toni): bundle these three params in
                         // VisualizerParams...
                         static_cast<VisualizationType>(FLAGS_viz_type),
                         backend_type_));
    //! Register input callbacks
    vio_backend_module_->registerOutputCallback(
        std::bind(&VisualizerModule::fillBackendQueue,
                  std::ref(*CHECK_NOTNULL(visualizer_module_.get())),
                  std::placeholders::_1));
    vio_frontend_module_->registerOutputCallback(
        std::bind(&VisualizerModule::fillFrontendQueue,
                  std::ref(*CHECK_NOTNULL(visualizer_module_.get())),
                  std::placeholders::_1));
    if (mesher_module_) {
      mesher_module_->registerOutputCallback(
          std::bind(&VisualizerModule::fillMesherQueue,
                    std::ref(*CHECK_NOTNULL(visualizer_module_.get())),
                    std::placeholders::_1));
    }
    //! Actual displaying of visual data is done in the main thread.
    display_module_ = VIO::make_unique<DisplayModule>(
        &display_input_queue_,
        nullptr,
        parallel_run_,
        // Use given displayer if any
        displayer
            ? std::move(displayer)
            : DisplayFactory::makeDisplay(DisplayType::kOpenCV,
                                          std::bind(&Pipeline::shutdown, this),
                                          OpenCv3dDisplayParams()));
  }

  if (FLAGS_use_lcd) {
    lcd_module_ = VIO::make_unique<LcdModule>(
        parallel_run_,
        LcdFactory::createLcd(LoopClosureDetectorType::BoW,
                              params.lcd_params_,
                              FLAGS_log_output));
    //! Register input callbacks
    vio_backend_module_->registerOutputCallback(
        std::bind(&LcdModule::fillBackendQueue,
                  std::ref(*CHECK_NOTNULL(lcd_module_.get())),
                  std::placeholders::_1));
    vio_frontend_module_->registerOutputCallback(
        std::bind(&LcdModule::fillFrontendQueue,
                  std::ref(*CHECK_NOTNULL(lcd_module_.get())),
                  std::placeholders::_1));
  }

  // All modules are ready, launch threads! If the parallel_run flag is set to
  // false this will not do anything.
  launchThreads();
}

/* -------------------------------------------------------------------------- */
Pipeline::~Pipeline() {
  LOG(INFO) << "Pipeline destructor called.";
  // Shutdown pipeline if it is not already down.
  if (!shutdown_) {
    shutdown();
  } else {
    LOG(INFO) << "Manual shutdown was requested.";
  }
}

/* -------------------------------------------------------------------------- */
void Pipeline::spinOnce(StereoImuSyncPacket::UniquePtr stereo_imu_sync_packet) {
  CHECK(stereo_imu_sync_packet);
  if (!shutdown_) {
    // Push to stereo frontend input queue.
    VLOG(2) << "Push input payload to Frontend.";
    stereo_frontend_input_queue_.pushBlockingIfFull(
        std::move(stereo_imu_sync_packet), 5u);

    if (!parallel_run_) {
      // Run the pipeline sequentially.
      spinSequential();
    }
  } else {
    LOG(WARNING) << "Not spinning pipeline as it's been shutdown.";
  }
}

// Returns whether the visualizer_ is running or not. While in parallel mode,
// it does not return unless shutdown.
bool Pipeline::spinViz() {
  if (display_module_) {
    return display_module_->spin();
  }
  return true;
}

/* -------------------------------------------------------------------------- */
void Pipeline::spinSequential() {
  // Spin once each pipeline module.
  // CHECK(data_provider_module_);
  // data_provider_module_->spin();

  CHECK(vio_frontend_module_);
  vio_frontend_module_->spin();

  CHECK(vio_backend_module_);
  vio_backend_module_->spin();

  if (mesher_module_) mesher_module_->spin();

  if (lcd_module_) lcd_module_->spin();

  if (visualizer_module_) visualizer_module_->spin();

  if (display_module_) display_module_->spin();
}

bool Pipeline::shutdownWhenFinished(const int& sleep_time_ms,
                                    const bool& print_stats) {
  // This is a very rough way of knowing if we have finished...
  // Since threads might be in the middle of processing data while we
  // query if the queues are empty.
  // Check every 0.5 seconds if all queues are empty.
  // Time to sleep between queries to the queues [in milliseconds].
  LOG(INFO) << "Shutting down VIO pipeline once processing has finished.";

  bool lcd_and_lcd_input_finished = true;
  if (lcd_module_) {
    lcd_and_lcd_input_finished = false;
  }

  CHECK(data_provider_module_);
  CHECK(vio_frontend_module_);
  CHECK(vio_backend_module_);

  while (
      !shutdown_ &&         // Loop while not explicitly shutdown.
      is_backend_ok_ &&     // Loop while backend is fine.
      (!isInitialized() ||  // Pipeline is not initialized and
                            // data is not yet consumed.
       !(!data_provider_module_->isWorking() &&
         (stereo_frontend_input_queue_.isShutdown() ||
          stereo_frontend_input_queue_.empty()) &&
         !vio_frontend_module_->isWorking() &&
         (backend_input_queue_.isShutdown() || backend_input_queue_.empty()) &&
         !vio_backend_module_->isWorking() &&
         (mesher_module_ ? !mesher_module_->isWorking() : true) &&
         (lcd_module_ ? !lcd_module_->isWorking() : true) &&
         (visualizer_module_ ? !visualizer_module_->isWorking() : true) &&
         (display_input_queue_.isShutdown() || display_input_queue_.empty()) &&
         (display_module_ ? !display_module_->isWorking() : true)))) {
    // Note that the values in the log below might be different than the
    // evaluation above since they are separately evaluated at different times.
    VLOG(1) << "shutdown_: " << shutdown_ << '\n'
            << "VIO pipeline status: \n"
            << "Pipeline initialized? " << isInitialized() << '\n'
            << "Frontend initialized? " << vio_frontend_module_->isInitialized()
            << '\n'
            << "Backend initialized? " << vio_backend_module_->isInitialized()
            << '\n'
            << "Data provider is working? "
            << data_provider_module_->isWorking() << '\n'
            << "Frontend input queue shutdown? "
            << stereo_frontend_input_queue_.isShutdown() << '\n'
            << "Frontend input queue empty? "
            << stereo_frontend_input_queue_.empty() << '\n'
            << "Frontend is working? " << vio_frontend_module_->isWorking()
            << '\n'
            << "Backend Input queue shutdown? "
            << backend_input_queue_.isShutdown() << '\n'
            << "Backend Input queue empty? " << backend_input_queue_.empty()
            << '\n'
            << "Backend is working? " << vio_backend_module_->isWorking()
            << '\n'
            << "Mesher is working? "
            << (mesher_module_ ? mesher_module_->isWorking() : false) << '\n'
            << "LCD is working? "
            << (lcd_module_ ? lcd_module_->isWorking() : false) << '\n'
            << "Visualizer is working? "
            << (visualizer_module_ ? visualizer_module_->isWorking() : false)
            << '\n'
            << "Display Input queue shutdown? "
            << display_input_queue_.isShutdown() << '\n'
            << "Display Input queue empty? " << display_input_queue_.empty()
            << '\n'
            << "Displayer is working? "
            << (display_module_ ? display_module_->isWorking() : false);

    VLOG_IF(5, mesher_module_)
        << "Mesher is working? " << mesher_module_->isWorking();

    VLOG_IF(5, lcd_module_)
        << "LoopClosureDetector is working? " << lcd_module_->isWorking();

    VLOG_IF(5, visualizer_module_)
        << "Visualizer is working? " << visualizer_module_->isWorking();

    VLOG_IF(5, display_module_)
        << "Visualizer is working? " << display_module_->isWorking();

    // Print all statistics
    if (print_stats) LOG(INFO) << utils::Statistics::Print();
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time_ms));

    if (!parallel_run_) {
      // Don't break, otw we will shutdown the pipeline.
      return false;
    }
  }
  LOG(INFO) << "Shutting down VIO, reason: input is empty and threads are "
               "idle.";
  VLOG(1) << "shutdown_: " << shutdown_ << '\n'
          << "VIO pipeline status: \n"
          << "Pipeline initialized? " << isInitialized() << '\n'
          << "Frontend initialized? " << vio_frontend_module_->isInitialized()
          << '\n'
          << "Backend initialized? " << vio_backend_module_->isInitialized()
          << '\n'
          << "Data provider is working? " << data_provider_module_->isWorking()
          << '\n'
          << "Frontend input queue shutdown? "
          << stereo_frontend_input_queue_.isShutdown() << '\n'
          << "Frontend input queue empty? "
          << stereo_frontend_input_queue_.empty() << '\n'
          << "Frontend is working? " << vio_frontend_module_->isWorking()
          << '\n'
          << "Backend Input queue shutdown? "
          << backend_input_queue_.isShutdown() << '\n'
          << "Backend Input queue empty? " << backend_input_queue_.empty()
          << '\n'
          << "Backend is working? " << vio_backend_module_->isWorking() << '\n'
          << "Mesher is working? "
          << (mesher_module_ ? mesher_module_->isWorking() : false) << '\n'
          << "LCD is working? "
          << (lcd_module_ ? lcd_module_->isWorking() : false) << '\n'
          << "Visualizer is working? "
          << (visualizer_module_ ? visualizer_module_->isWorking() : false)
          << '\n'
          << "Display Input queue shutdown? "
          << display_input_queue_.isShutdown() << '\n'
          << "Display Input queue empty? " << display_input_queue_.empty()
          << '\n'
          << "Displayer is working? "
          << (display_module_ ? display_module_->isWorking() : false);
  if (!shutdown_) shutdown();
  return true;
}

/* -------------------------------------------------------------------------- */
void Pipeline::shutdown() {
  LOG_IF(ERROR, shutdown_) << "Shutdown requested, but Pipeline was already "
                              "shutdown.";
  LOG(INFO) << "Shutting down VIO pipeline.";
  shutdown_ = true;

  // First: call registered shutdown callbacks, these are typically to signal
  // data providers that they should now die.
  if (shutdown_pipeline_cb_) {
    LOG(INFO) << "Calling registered shutdown callbacks...";
    // Mind that this will raise a SIGSEGV seg fault if the callee is destroyed.
    shutdown_pipeline_cb_();
  }

  // Second: stop data provider
  CHECK(data_provider_module_);
  data_provider_module_->shutdown();

  // Third: stop VIO's threads
  stopThreads();
  if (parallel_run_) {
    joinThreads();
  }
  LOG(INFO) << "VIO Pipeline's threads shutdown successfully.\n"
            << "VIO Pipeline successful shutdown.";
}

/* -------------------------------------------------------------------------- */
void Pipeline::launchThreads() {
  if (parallel_run_) {
    frontend_thread_ = VIO::make_unique<std::thread>(
        &StereoVisionFrontEndModule::spin,
        CHECK_NOTNULL(vio_frontend_module_.get()));

    backend_thread_ = VIO::make_unique<std::thread>(
        &VioBackEndModule::spin, CHECK_NOTNULL(vio_backend_module_.get()));

    if (mesher_module_) {
      mesher_thread_ = VIO::make_unique<std::thread>(
          &MesherModule::spin, CHECK_NOTNULL(mesher_module_.get()));
    }

    if (lcd_module_) {
      lcd_thread_ = VIO::make_unique<std::thread>(
          &LcdModule::spin, CHECK_NOTNULL(lcd_module_.get()));
    }

    if (visualizer_module_) {
      visualizer_thread_ = VIO::make_unique<std::thread>(
          &VisualizerModule::spin, CHECK_NOTNULL(visualizer_module_.get()));
    }
    LOG(INFO) << "Pipeline Modules launched (parallel_run set to "
              << parallel_run_ << ").";
  } else {
    LOG(INFO) << "Pipeline Modules running in sequential mode"
              << " (parallel_run set to " << parallel_run_ << ").";
  }
}

/* -------------------------------------------------------------------------- */
// Resume all workers and queues
void Pipeline::resume() {
  LOG(INFO) << "Restarting frontend workers and queues...";
  stereo_frontend_input_queue_.resume();

  LOG(INFO) << "Restarting backend workers and queues...";
  backend_input_queue_.resume();
}

/* -------------------------------------------------------------------------- */
void Pipeline::stopThreads() {
  VLOG(1) << "Stopping workers and queues...";

  backend_input_queue_.shutdown();
  CHECK(vio_backend_module_);
  vio_backend_module_->shutdown();

  stereo_frontend_input_queue_.shutdown();
  CHECK(vio_frontend_module_);
  vio_frontend_module_->shutdown();

  if (mesher_module_) mesher_module_->shutdown();
  if (lcd_module_) lcd_module_->shutdown();
  if (visualizer_module_) visualizer_module_->shutdown();
  if (display_module_) {
    display_input_queue_.shutdown();
    display_module_->shutdown();
  }

  VLOG(1) << "Sent stop flag to all module and queues...";
}

/* -------------------------------------------------------------------------- */
void Pipeline::joinThreads() {
  LOG_IF(WARNING, !parallel_run_)
      << "Asked to join threads while in sequential mode, this is ok, but "
      << "should not happen.";
  VLOG(1) << "Joining threads...";

  joinThread("backend", backend_thread_.get());
  joinThread("frontend", frontend_thread_.get());
  joinThread("mesher", mesher_thread_.get());
  joinThread("lcd", lcd_thread_.get());
  joinThread("visualizer", visualizer_thread_.get());

  VLOG(1) << "All threads joined.";
}

void Pipeline::joinThread(const std::string& thread_name, std::thread* thread) {
  if (thread) {
    VLOG(1) << "Joining " << thread_name.c_str() << " thread...";
    if (thread->joinable()) {
      thread->join();
      VLOG(1) << "Joined " << thread_name.c_str() << " thread...";
    } else {
      LOG_IF(ERROR, parallel_run_)
          << thread_name.c_str() << " thread is not joinable...";
    }
  } else {
    LOG(WARNING) << "No " << thread_name.c_str() << " thread, not joining.";
  }
}

}  // namespace VIO
