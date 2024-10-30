#include "glomap/estimators/relpose_estimation.h"

#include <colmap/util/threading.h>

#include <PoseLib/robust.h>

namespace glomap {

void EstimateRelativePoses(const colmap::Database& database,
                           ViewGraph& view_graph,
                           std::unordered_map<camera_t, Camera>& cameras,
                           std::unordered_map<image_t, Image>& images,
                           const RelativePoseEstimationOptions& options) {
  std::vector<image_pair_t> valid_pair_ids;
  for (auto& [image_pair_id, image_pair] : view_graph.image_pairs) {
    if (!image_pair.is_valid) continue;
    valid_pair_ids.push_back(image_pair_id);
  }

  const int64_t num_image_pairs = valid_pair_ids.size();
  const int64_t kNumChunks = 10;
  const int64_t interval =
      std::ceil(static_cast<double>(num_image_pairs) / kNumChunks);

  // multi-threading management. More than one creates problems when accessing database.
  // old
  // colmap::ThreadPool thread_pool(colmap::ThreadPool::kMaxNumThreads);
  // new
  // colmap::ThreadPool thread_pool(options.load_poses ? 1 : colmap::ThreadPool::kMaxNumThreads); 
  // fancy
  int num_threads = 1;
  if (!options.load_poses){
    num_threads = colmap::ThreadPool::kMaxNumThreads;
  }
  colmap::ThreadPool thread_pool(num_threads);
  

  LOG(INFO) << "Estimating relative pose for " << num_image_pairs << " pairs";
  for (int64_t chunk_id = 0; chunk_id < kNumChunks; chunk_id++) {
    std::cout << "\r Estimating relative pose: " << chunk_id * kNumChunks << "%"
              << std::flush;
    const int64_t start = chunk_id * interval;
    const int64_t end =
        std::min<int64_t>((chunk_id + 1) * interval, num_image_pairs);

    for (int64_t pair_idx = start; pair_idx < end; pair_idx++) {
      thread_pool.AddTask([&, pair_idx]() {
        // Define as thread-local to reuse memory allocation in different tasks.
        thread_local std::vector<Eigen::Vector2d> points2D_1;
        thread_local std::vector<Eigen::Vector2d> points2D_2;
        thread_local std::vector<char> inliers;

        ImagePair& image_pair =
            view_graph.image_pairs[valid_pair_ids[pair_idx]];
        const Image& image1 = images[image_pair.image_id1];
        const Image& image2 = images[image_pair.image_id2];
        const Eigen::MatrixXi& matches = image_pair.matches;

        // Collect the original 2D points
        points2D_1.clear();
        points2D_2.clear();
        for (size_t idx = 0; idx < matches.rows(); idx++) {
          points2D_1.push_back(image1.features[matches(idx, 0)]);
          points2D_2.push_back(image2.features[matches(idx, 1)]);
        }

        inliers.clear();

        if (!options.load_poses) {
          poselib::CameraPose pose_rel_calc;
          try {
            poselib::estimate_relative_pose(
                points2D_1,
                points2D_2,
                ColmapCameraToPoseLibCamera(cameras[image1.camera_id]),
                ColmapCameraToPoseLibCamera(cameras[image2.camera_id]),
                options.ransac_options,
                options.bundle_options,
                &pose_rel_calc,
                &inliers);
          } catch (const std::exception& e) {
            LOG(ERROR) << "Error in relative pose estimation: " << e.what();
            image_pair.is_valid = false;
            return;
          }

          // Convert the relative pose to the glomap format | (w,x,y,z) -> (x,y,z,w)
          for (int i = 0; i < 4; i++) {
            image_pair.cam2_from_cam1.rotation.coeffs()[i] =
                pose_rel_calc.q[(i + 1) % 4];
          }
          image_pair.cam2_from_cam1.translation = pose_rel_calc.t;

        }else{
          // load poses from db
          colmap::TwoViewGeometry pose = 
              database.ReadTwoViewGeometry(
                image_pair.image_id1, image_pair.image_id2);
          image_pair.cam2_from_cam1.rotation = pose.cam2_from_cam1.rotation;
          image_pair.cam2_from_cam1.translation = pose.cam2_from_cam1.translation;
        }
      });
    }

    thread_pool.Wait();
  }

  std::cout << "\r Estimating relative pose: 100%" << std::endl;
  LOG(INFO) << "Estimating relative pose done";
}

}  // namespace glomap
