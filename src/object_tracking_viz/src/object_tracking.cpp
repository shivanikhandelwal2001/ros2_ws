#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/string.hpp>
#include <nlohmann/json.hpp>
#include <eigen3/Eigen/Dense>
#include <unordered_map>
#include <set>
#include <vector>
#include <cmath>
#include <string>

using json = nlohmann::json;


// Centroid-based Object Tracker for tracking detected objects across frames
class CentroidTracker {
public:
    CentroidTracker(int max_disappeared, double dist_thresh)
        : max_disappeared_(max_disappeared), dist_thresh_(dist_thresh), next_object_id_(0) {}

    // Updates the tracker with new detections and returns the tracked object positions
    std::unordered_map<int, Eigen::Vector2d> update(const std::vector<std::vector<int>>& detections) {
        if (detections.empty()) {
            // Increment disappearance count for tracked objects if no detections
            for (auto it = objects_.begin(); it != objects_.end();) {
                int object_id = it->first;
                disappeared_[object_id]++;
                if (disappeared_[object_id] > max_disappeared_) {
                    disappeared_.erase(object_id);
                    it = objects_.erase(it);
                } else {
                    ++it;
                }
            }
            return objects_;
        }

        // Convert bounding boxes to centroids
        std::vector<Eigen::Vector2d> new_centroids;
        for (const auto& d : detections) {
            int x1 = d[0], y1 = d[1], x2 = d[2], y2 = d[3];
            new_centroids.emplace_back((x1 + x2) / 2, (y1 + y2) / 2);
        }
        
        // If no objects are currently tracked, register all new detections
        if (objects_.empty()) {
            for (const auto& centroid : new_centroids) {
                objects_[next_object_id_] = centroid;
                disappeared_[next_object_id_] = 0;
                next_object_id_++;
            }
            return objects_;
        }

        // Compute Euclidean distance matrix between tracked objects and new detections
        int num_objects = objects_.size();
        int num_detections = new_centroids.size();
        Eigen::MatrixXd distances(num_objects, num_detections);

        std::vector<int> object_ids;
        std::vector<Eigen::Vector2d> object_centroids;

        for (const auto& pair : objects_) {
            object_ids.push_back(pair.first);
            object_centroids.push_back(pair.second);
        }

        for (int i = 0; i < num_objects; ++i) {
            for (int j = 0; j < num_detections; ++j) {
                distances(i, j) = (object_centroids[i] - new_centroids[j]).norm();
            }
        }

        std::set<int> used_detections;

        // Assign existing objects to the nearest detected objects
        for (int i = 0; i < num_objects; ++i) {
            int min_idx;
            double min_dist = distances.row(i).minCoeff(&min_idx);

            if (min_dist > dist_thresh_) {
                // Mark object as disappeared if no close detection is found
                disappeared_[object_ids[i]]++;
                if (disappeared_[object_ids[i]] > max_disappeared_) {
                    disappeared_.erase(object_ids[i]);
                    objects_.erase(object_ids[i]);
                }
            } else {
                // Update object position with new detection
                objects_[object_ids[i]] = new_centroids[min_idx];
                disappeared_[object_ids[i]] = 0;
                used_detections.insert(min_idx);
            }
        }

        // Register new objects for unmatched detections
        for (int i = 0; i < num_detections; ++i) {
            if (used_detections.find(i) == used_detections.end()) {
                objects_[next_object_id_] = new_centroids[i];
                disappeared_[next_object_id_] = 0;
                next_object_id_++;
            }
        }

        return objects_;
    }

private:
    int max_disappeared_; // Max frames an object can disappear before being removed
    double dist_thresh_;  // Distance threshold for tracking updates
    int next_object_id_;  // Next available object ID
    std::unordered_map<int, Eigen::Vector2d> objects_; // Stores active tracked objects
    std::unordered_map<int, int> disappeared_; // Tracks the disappearance count of objects
};


// ROS2 Node for subscribing to object detections and publishing tracked object data
class TrackingNode : public rclcpp::Node {
public:
    TrackingNode() : Node("object_tracking"), tracker_(50, 50.0) {

        this->declare_parameter<int>("max_disappeared", 50);
        this->declare_parameter<double>("dist_thresh", 50.0);

        int max_disappeared = this->get_parameter("max_disappeared").as_int();
        double dist_thresh = this->get_parameter("dist_thresh").as_double();
        
        tracker_ = CentroidTracker(max_disappeared, dist_thresh);

        detection_sub_ = this->create_subscription<std_msgs::msg::String>(
            "/object_detection", 10, std::bind(&TrackingNode::detection_callback, this, std::placeholders::_1));

        tracking_pub_ = this->create_publisher<std_msgs::msg::String>("/object_tracking", 10);
    }

private:
    // Callback function to process incoming detection messages
    void detection_callback(const std_msgs::msg::String::SharedPtr msg) {
        json detections_json = json::parse(msg->data);
        std::vector<std::vector<int>> detections;

        // Extract bounding boxes from the JSON message
        for (const auto& item : detections_json) {
            std::vector<int> bbox = item["bbox"];
            detections.push_back(bbox);
        }

        // Update tracker with new detections
        auto tracked_objects = tracker_.update(detections);

        json tracking_json;
        for (const auto& pair : tracked_objects) {
            tracking_json[std::to_string(pair.first)] = { pair.second[0], pair.second[1] };
        }

        // Publish tracked object data
        std_msgs::msg::String tracking_msg;
        tracking_msg.data = tracking_json.dump();
        tracking_pub_->publish(tracking_msg);

        RCLCPP_INFO(this->get_logger(), "Tracked %ld objects", tracked_objects.size());
    }

    rclcpp::Subscription<std_msgs::msg::String>::SharedPtr detection_sub_;
    rclcpp::Publisher<std_msgs::msg::String>::SharedPtr tracking_pub_;
    CentroidTracker tracker_;
};


// Main function to initialize and run the ROS2 node
int main(int argc, char** argv) {
    rclcpp::init(argc, argv);
    auto node = std::make_shared<TrackingNode>();
    rclcpp::spin(node);
    rclcpp::shutdown();
    return 0;
}
