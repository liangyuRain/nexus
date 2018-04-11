#include <boost/filesystem.hpp>
#include <cmath>
#include <fstream>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <iostream>
#include <cstdlib>
#include <string>
#include <time.h>
#include <thread>
#include <vector>
#include <unordered_map>
#include <yaml-cpp/yaml.h>

#include "nexus/common/device.h"
#include "nexus/common/block_queue.h"
#include "nexus/common/model_db.h"
#include "nexus/backend/model_ins.h"
#include "nexus/proto/nnquery.pb.h"

DEFINE_int32(gpu, 0, "GPU device id");
DEFINE_string(framework, "", "Framework");
DEFINE_string(model, "", "Model name");
DEFINE_int32(model_version, 1, "Version");
DEFINE_string(model_root, "", "Model root directory");
DEFINE_string(image_dir, "", "Image directory");
DEFINE_int32(min_batch, 1, "Minimum batch size");
DEFINE_int32(max_batch, 256, "Maximum batch size");
DEFINE_string(output, "", "Output file");
DEFINE_int32(height, 0, "Image height");
DEFINE_int32(width, 0, "Image width");
DEFINE_int32(repeat, 10, "Repeat times for profiling");

namespace nexus {
namespace backend {

using duration = std::chrono::microseconds;
namespace fs = boost::filesystem;

class ModelProfiler {
 public:
  ModelProfiler(int gpu, const std::string& framework,
                const std::string& model_name, int model_version,
                const std::string& image_dir, int height=0, int width=0) :
      gpu_(gpu),
      model_info_(ModelDatabase::Singleton().GetModelInfo(
          framework, model_name, model_version)) {
    // Init model session
    model_sess_.set_framework(framework);
    model_sess_.set_model_name(model_name);
    model_sess_.set_version(model_version);
    model_sess_.set_latency_sla(50000);
    if (height > 0) {
      CHECK_GT(width, 0) << "Height and width must be set together";
      model_sess_.set_image_height(height);
      model_sess_.set_image_width(width);
    } else {
      if (model_info_["resizable"] && model_info_["resizable"].as<bool>()) {
        // Set default image size for resizable CNN
        model_sess_.set_image_height(
            model_info_["image_height"].as<uint32_t>());
        model_sess_.set_image_width(
            model_info_["image_width"].as<uint32_t>());
      }
    }
    LOG(INFO) << "Profile model " << ModelSessionToProfileID(model_sess_);
    // Get test dataset
    ListImages(image_dir);
    // Init GPU device
    NEXUS_CUDA_CHECK(cudaSetDevice(gpu_));
    gpu_device_ = DeviceManager::Singleton().GetGPUDevice(gpu_);
  }

  void Profile(int min_batch, int max_batch, const std::string output="",
               int repeat=10) {
    // Get model info
    size_t origin_freemem = gpu_device_->FreeMemory();
    std::vector<uint64_t> preprocess_lats;
    std::vector<uint64_t> postprocess_lats;
    std::unordered_map<int, std::tuple<float, float, size_t> > forward_stats;
    ModelInstanceDesc desc;
    desc.mutable_model_session()->CopyFrom(model_sess_);
    BlockPriorityQueue<Task> task_queue;

    // preprocess
    std::vector<std::shared_ptr<Task> > tasks;
    std::vector<std::vector<ArrayPtr> > batch_inputs;
    {
      desc.set_batch(1);
      desc.set_max_batch(1);
      auto model = CreateModelInstance(gpu_, desc, model_info_, task_queue);
      // prepare the input
      int num_inputs = max_batch * (repeat + 1);
      if (num_inputs > 1000) {
        num_inputs = 1000;
      }
      for (int i = 0; i < num_inputs; ++i) {
        // Load input image
        int idx = rand() % test_images_.size();
        std::string im;
        ReadImage(test_images_[idx], &im);
        auto task = std::make_shared<Task>();
        task->SetDeadline(std::chrono::milliseconds(100000));
        auto input = task->query.mutable_input();
        input->set_data_type(DT_IMAGE);
        auto image = input->mutable_image();
        image->set_data(im);
        image->set_format(ImageProto::JPEG);
        image->set_color(true);
        std::vector<ArrayPtr> input_arrays;
        auto beg = std::chrono::high_resolution_clock::now();
        model->PreprocessImpl(task, &input_arrays);
        auto end = std::chrono::high_resolution_clock::now();
        batch_inputs.push_back(input_arrays);
        tasks.push_back(task);
        if (i > 0) {
          preprocess_lats.push_back(
              std::chrono::duration_cast<duration>(end - beg).count());
        }
      }
    }

    // forward and postprocess
    for (int batch = min_batch; batch <= max_batch; ++batch) {
      desc.set_batch(batch);
      desc.set_max_batch(batch);
      auto model = CreateModelInstance(gpu_, desc, model_info_, task_queue);
      // latencies
      std::vector<uint64_t> forward_lats;
      for (int i = 0; i < batch * (repeat + 1); ++i) {
        int idx = i % batch_inputs.size();
        auto task = std::make_shared<Task>();
        task->query.set_query_id(i);
        task->SetDeadline(std::chrono::milliseconds(100000));
        task->attrs = tasks[idx]->attrs;
        model->AppendInputs(task, batch_inputs[idx]);
      }
      // dry run
      model->Forward();
      // start meansuring forward latency
      for (int i = 0; i < repeat; ++i) {
        auto beg = std::chrono::high_resolution_clock::now();
        model->Forward();
        auto end = std::chrono::high_resolution_clock::now();
        forward_lats.push_back(
            std::chrono::duration_cast<duration>(end - beg).count());
      }
      size_t curr_freemem = gpu_device_->FreeMemory();
      size_t memory_usage = origin_freemem - curr_freemem;
      LOG(INFO) << "memory usage: " << memory_usage;
      for (int i = 0; i < batch * (repeat + 1); ++i) {
        auto task = task_queue.pop();
        auto beg = std::chrono::high_resolution_clock::now();
        model->Postprocess(task);
        auto end = std::chrono::high_resolution_clock::now();
        if (i > 0 && postprocess_lats.size() < 2000) {
          postprocess_lats.push_back(
              std::chrono::duration_cast<duration>(end - beg).count());
        }
      }
      float mean, std;
      std::tie(mean, std) = GetStats<uint64_t>(forward_lats);
      forward_stats.emplace(batch, std::make_tuple(mean, std, memory_usage));
      CHECK_EQ(task_queue.size(), 0) << "Task queue is not empty";
    }
    // output to file
    std::ostream* fout;
    if (output.length() == 0) {
      fout = &std::cout;
    } else {
      fout = new std::ofstream(output, std::ofstream::out);
    }
  
    *fout << ModelSessionToProfileID(model_sess_) << "\n";
    *fout << gpu_device_->device_name() << "\n";
    *fout << "Forward latency\n";
    *fout << "batch,latency(us),std(us),memory(B)\n";
    for (int batch = min_batch; batch <= max_batch; ++batch) {
      float mean, std;
      size_t memory_usage;
      std::tie(mean, std, memory_usage) = forward_stats.at(batch);
      *fout << batch << "," << mean << "," << std << "," << memory_usage << "\n";
    }
    float mean, std;
    std::tie(mean, std) = GetStats<uint64_t>(preprocess_lats);
    *fout << "Preprocess latency\nmean(us),std(us)\n";
    *fout << mean << "," << std << "\n";
    std::tie(mean, std) = GetStats<uint64_t>(postprocess_lats);
    *fout << "Postprocess latency\nmean(us),std(us)\n";
    *fout << mean << "," << std << "\n";
    if (fout != &std::cout) {
      delete fout;
    }
  }

 private:
  template<class T>
  std::pair<float, float> GetStats(const std::vector<T>& lats) {
    float mean = 0.;
    float std = 0.;
    for (uint i = 0; i < lats.size(); ++i) {
      mean += lats[i];
    }
    mean /= lats.size();
    for (uint i = 0; i < lats.size(); ++i) {
      std += (lats[i] - mean) * (lats[i] - mean);
    }
    std = sqrt(std / (lats.size() - 1));
    return {mean, std};
  }

  void ListImages(const std::string& root_dir) {
    fs::directory_iterator end_iter;
    for (fs::directory_iterator it(root_dir); it != end_iter; ++it) {
      test_images_.push_back(it->path().string());
    }
    LOG(INFO) << "Number of test images: " << test_images_.size();
  }

  void ReadImage(const std::string& file_path, std::string* content) {
    std::ifstream fin(file_path, std::ios::binary | std::ios::ate);
    size_t fsize = fin.tellg();
    fin.seekg(0);
    content->resize(fsize);
    fin.read(&((*content)[0]), fsize);
    fin.close();
  }

 private:
  int gpu_;
  ModelSession model_sess_;
  const YAML::Node& model_info_;
  std::string framework_;
  std::string model_name_;
  int version_;
  int height_;
  int width_;
  std::vector<std::string> test_images_;
  GPUDevice* gpu_device_;
};

} // namespace backend
} // namespace nexus


int main(int argc, char** argv) {
  using namespace nexus;
  using namespace nexus::backend;
  
  // log to stderr
  FLAGS_logtostderr = 1;
  // Init glog
  google::InitGoogleLogging(argv[0]);
  // Parse command line flags
  google::ParseCommandLineFlags(&argc, &argv, true);
  // Setup backtrace on segfault
  google::InstallFailureSignalHandler();
  if (FLAGS_model_root.length() == 0) {
    LOG(FATAL) << "Missing model_root";
  }
  if (FLAGS_framework.length() == 0) {
    LOG(FATAL) << "Missing framework";
  }
  if (FLAGS_model.length() == 0) {
    LOG(FATAL) << "Missing model";
  }
  if (FLAGS_image_dir.length() == 0) {
    LOG(FATAL) << "Missing image_dir";
  }
  srand(time(NULL));
  ModelDatabase::Singleton().Init(FLAGS_model_root);
  ModelProfiler profiler(FLAGS_gpu, FLAGS_framework, FLAGS_model,
                         FLAGS_model_version, FLAGS_image_dir, FLAGS_height,
                         FLAGS_width);
  profiler.Profile(FLAGS_min_batch, FLAGS_max_batch, FLAGS_output,
                   FLAGS_repeat);
}