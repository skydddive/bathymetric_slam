/* Copyright 2019 Ignacio Torroba (torroba@kth.se)
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <cereal/archives/binary.hpp>

#include <pcl/filters/uniform_sampling.h>

#include "data_tools/std_data.h"
#include "data_tools/benchmark.h"

#include "submaps_tools/cxxopts.hpp"
#include "submaps_tools/submaps.hpp"

#include "registration/utils_visualization.hpp"
#include "registration/gicp_reg.hpp"

#include "graph_optimization/utils_g2o.hpp"
#include "graph_optimization/graph_construction.hpp"
#include "graph_optimization/ceres_optimizer.hpp"
#include "graph_optimization/read_g2o.h"

#include "bathy_slam/bathy_slam.hpp"

#define INTERACTIVE 0
#define VISUAL 1

using namespace Eigen;
using namespace std;
using namespace g2o;

int main(int argc, char** argv){

    // Inputs
    std::cout << "------------------------------------------------------------" << std::endl;
    std::string folder_str, path_str, output_str, original, simulation, const_cov, mc_method, method;
    cxxopts::Options options("MyProgram", "One line description of MyProgram");
    options.add_options()
        ("help", "Print help")
        ("method", "Monte Carlo covs", cxxopts::value(method))
        ("covs_folder", "Input covs folder", cxxopts::value(folder_str))
        ("output_cereal", "Output graph cereal", cxxopts::value(output_str))
        ("original", "Disturb original trajectory", cxxopts::value(original))
        ("simulation", "Simulation data from Gazebo", cxxopts::value(simulation))
        ("const_cov", "Constant covariance value", cxxopts::value(const_cov))
        ("mc", "Monte Carlo covs", cxxopts::value(mc_method))
        ("slam_cereal", "Input ceres file", cxxopts::value(path_str));

    auto result = options.parse(argc, argv);
    if (result.count("help")) {
        cout << options.help({ "", "Group" }) << endl;
        exit(0);
    }
    if(output_str.empty()){
        output_str = "output_cereal.cereal";
    }
    boost::filesystem::path output_path(output_str);
    string outFilename = "graph_corrupted.g2o";   // G2O output file

    // Parse submaps from cereal file
    SubmapsVec submaps_gt;
    boost::filesystem::path submaps_path(path_str);
    std::cout << "Input data " << boost::filesystem::basename(submaps_path) << std::endl;
    if(simulation == "yes"){
        submaps_gt = readSubmapsInDir(submaps_path.string());
    }
    else{
        if(original == "yes"){
            std_data::pt_submaps ss = std_data::read_data<std_data::pt_submaps>(submaps_path);
            submaps_gt = parseSubmapsAUVlib(ss);
        }
        else{
            std::ifstream is(boost::filesystem::basename(submaps_path) + ".cereal", std::ifstream::binary);
            {
              cereal::BinaryInputArchive iarchive(is);
              iarchive(submaps_gt);
            }
        }
        // Filtering of submaps
        PointCloudT::Ptr cloud_ptr (new PointCloudT);
        pcl::UniformSampling<PointT> us_filter;
        us_filter.setInputCloud (cloud_ptr);
        us_filter.setRadiusSearch(2);   // 1 for Borno, 2 for Antarctica
        for(SubmapObj& submap_i: submaps_gt){
    //        std::cout << "before " << submap_i.submap_pcl_.size() << std::endl;
            *cloud_ptr = submap_i.submap_pcl_;
            us_filter.setInputCloud(cloud_ptr);
            us_filter.filter(*cloud_ptr);
            submap_i.submap_pcl_ = *cloud_ptr;
    //        std::cout << submap_i.submap_pcl_.size() << std::endl;
        }
    }

    // Survey distance
    float dist = 0;
    int i = 0;
    for(SubmapObj& submap_i: submaps_gt){
        i++;
        if(i==submaps_gt.size()){
            break;
        }
        dist += (submap_i.submap_tf_.translation() - submaps_gt.at(i).submap_tf_.translation()).norm();
    }
    std::cout << "Trajectory (m) " << dist << std::endl;

    // Read training covs from folder
    covs covs_lc;
    std::string results_path;
    boost::filesystem::path folder(folder_str);
    if(boost::filesystem::is_directory(folder)) {
        covs_lc = readCovsFromFiles(folder);
        if(mc_method == "yes"){
            results_path = "results_mc.txt";
        }
        else{
            results_path = "results_nn.txt";
        }
        std::cout << "Results to " << results_path << std::endl;
    }
    else{
        if(const_cov.empty()){
            std::cout << "Input a covariance value for the optimization" << std::endl;
            exit(0);
        }
        Eigen::Vector2d diag;
        diag << std::stod(const_cov), std::stod(const_cov);
        covs_lc.push_back(diag.asDiagonal());
        results_path = "results_" + const_cov + ".txt";
    }

    // Benchmark GT
    benchmark::track_error_benchmark benchmark("real_data");
    PointsT gt_map = pclToMatrixSubmap(submaps_gt);
    PointsT gt_track = trackToMatrixSubmap(submaps_gt);
    benchmark.add_ground_truth(gt_map, gt_track);
    ceres::optimizer::saveOriginalTrajectory(submaps_gt); // Save original trajectory to txt

    // Visualization
#if VISUAL == 1
    PCLVisualizer viewer ("Submaps viewer");
    SubmapsVisualizer* visualizer = new SubmapsVisualizer(viewer);
    visualizer->setVisualizer(submaps_gt, 1);
    while(!viewer.wasStopped ()){
        viewer.spinOnce ();
    }
    viewer.resetStoppedFlag();
#endif

    // GICP reg for submaps
    SubmapRegistration gicp_reg;

    // Graph constructor
    GraphConstructor graph_obj(covs_lc);
    graph_obj.edge_covs_type_ = std::stoi(method);

    // Noise generators
    GaussianGen transSampler, rotSampler;
    Matrix<double, 6,6> information = generateGaussianNoise(transSampler, rotSampler);

//    // Noise to map
//    addNoiseToMap(transSampler, rotSampler, submaps_gt);
//    visualizer->updateVisualizer(submaps_gt);
//    while(!viewer.wasStopped ()){
//        viewer.spinOnce ();
//    }
//    viewer.resetStoppedFlag();

    // Create SLAM solver and run offline
    std::cout << "Running graph SLAM " << std::endl;
    BathySlam slam_solver(graph_obj, gicp_reg);
    SubmapsVec submaps_reg = slam_solver.runOffline(submaps_gt, transSampler, rotSampler);

#if VISUAL == 1
    // Update visualizer
    visualizer->updateVisualizer(submaps_reg);
    while(!viewer.wasStopped ()){
        viewer.spinOnce ();
    }
    viewer.resetStoppedFlag();
#endif

    // Add noise to edges on the graph
    graph_obj.addNoiseToGraph(transSampler, rotSampler);

    // Create initial DR chain and visualize
    graph_obj.createInitialEstimate(submaps_reg);

#if VISUAL == 1
    visualizer->plotPoseGraphG2O(graph_obj, submaps_reg);
    while(!viewer.wasStopped ()){
        viewer.spinOnce ();
    }
    viewer.resetStoppedFlag();
#endif

    // Save graph to output g2o file (optimization can be run with G2O)
    graph_obj.saveG2OFile(outFilename);

    // Benchmar corrupted
    PointsT reg_map = pclToMatrixSubmap(submaps_reg);
    PointsT reg_track = trackToMatrixSubmap(submaps_reg);
    benchmark.add_benchmark(reg_map, reg_track, "corrupted");

    // Optimize graph and save to cereal
    google::InitGoogleLogging(argv[0]);
    std::tuple<ceres::optimizer::MapOfPoses, int> opt_results;
    opt_results = ceres::optimizer::ceresSolver(outFilename, graph_obj.drEdges_.size());
    ceres::optimizer::updateSubmapsCeres(std::get<0>(opt_results), submaps_reg);
    std::cout << "Output cereal: " << boost::filesystem::basename(output_path) << std::endl;
    std::ofstream os(boost::filesystem::basename(output_path) + ".cereal", std::ofstream::binary);
    {
        cereal::BinaryOutputArchive oarchive(os);
        oarchive(submaps_reg);
        os.close();
    }

#if VISUAL == 1
    // Visualize Ceres output
    visualizer->plotPoseGraphCeres(submaps_reg);
    while(!viewer.wasStopped ()){
        viewer.spinOnce ();
    }
    viewer.resetStoppedFlag();
    delete(visualizer);
#endif

    // Benchmark Optimized
    PointsT opt_map = pclToMatrixSubmap(submaps_reg);
    PointsT opt_track = trackToMatrixSubmap(submaps_reg);
    benchmark.add_benchmark(opt_map, opt_track, "optimized");
    benchmark.print_summary();

    // Save number of iterations
    ofstream fileOutputStream;
    if (outFilename != "-") {
      fileOutputStream.open(results_path.c_str(), std::ios::in | std::ios::out | std::ios::ate);
    } else {
      cerr << "writing to stdout" << endl;
    }
    std::cout << "Results to " << results_path << std::endl;

    ostream& fout = outFilename != "-" ? fileOutputStream : std::cout;
    fout << std::endl;
    fout << std::get<1>(opt_results) << " ";
    fileOutputStream.close();

    // Save benchmark results
//    benchmark.save_summary(results_path);

    // Save ETA resuls
    std::string command_str = "./plot_results.py --initial_poses poses_original.txt "
                              "--corrupted_poses poses_corrupted.txt --optimized_poses poses_optimized.txt "
                              "--output_file " + results_path;

    const char *command = command_str.c_str();
    system(command);

    return 0;
}
