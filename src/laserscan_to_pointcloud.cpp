/**\file LaserScanToPointcloud.cpp
 * \brief Implementation of a PointCloud2 builder from LaserScans.
 *
 * @version 1.0
 * @author Carlos Miguel Correia da Costa
 */

// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>   <includes>   <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<
#include <laserscan_to_pointcloud/laserscan_to_pointcloud.h>
// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>   </includes>  <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<


// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>   <imports>   <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<
// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>   </imports>  <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<


namespace laserscan_to_pointcloud {
// =============================================================================  <public-section>   ===========================================================================
// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>   <constructors-destructor>   <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<
LaserScanToPointcloud::LaserScanToPointcloud(std::string target_frame, double min_range_cutoff_percentage, double max_range_cutoff_percentage, int number_of_tf_queries_for_spherical_interpolation, double tf_lookup_timeout) :
		target_frame_(target_frame),
		min_range_cutoff_percentage_offset_(min_range_cutoff_percentage), max_range_cutoff_percentage_offset_(max_range_cutoff_percentage),
		tf_lookup_timeout_(tf_lookup_timeout),
		remove_invalid_measurements_(true),
		number_of_tf_queries_for_spherical_interpolation_(number_of_tf_queries_for_spherical_interpolation),
		number_of_pointclouds_created_(0),
		number_of_points_in_cloud_(0),
		number_of_scans_assembled_in_current_pointcloud_(0),
		polar_to_cartesian_matrix_angle_min_(0), polar_to_cartesian_matrix_angle_max_(0), polar_to_cartesian_matrix_angle_increment_(0) {

	polar_to_cartesian_matrix_.resize(Eigen::NoChange, 0);
}

LaserScanToPointcloud::~LaserScanToPointcloud() {}
// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>   </constructors-destructor>  <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<



// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>   <LaserScanToPointcloud-functions>   <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<
bool LaserScanToPointcloud::updatePolarToCartesianProjectionMatrix(const sensor_msgs::LaserScanConstPtr& laser_scan) {
	size_t number_of_scan_points = laser_scan->ranges.size();
	if (polar_to_cartesian_matrix_.cols() != number_of_scan_points
			/*|| polar_to_cartesian_matrix_angle_min_ != laser_scan->angle_min
			|| polar_to_cartesian_matrix_angle_max_ != laser_scan->angle_max
			|| polar_to_cartesian_matrix_angle_increment_ != laser_scan->angle_increment*/) { // todo: fix float compare

		ROS_DEBUG_STREAM("Updating polar to cartesian projection matrix with ->" \
				<< "\n\t[ranges.size()]:" << laser_scan->ranges.size() \
				<< "\n\t[angle_min]:" << laser_scan->angle_min \
				<< "\n\t[angle_max]:" << laser_scan->angle_max
				<< "\n\t[increment]:" << laser_scan->angle_increment);

		// recompute sin and cos values
		polar_to_cartesian_matrix_.resize(Eigen::NoChange, laser_scan->ranges.size());
		polar_to_cartesian_matrix_angle_min_ = laser_scan->range_min;
		polar_to_cartesian_matrix_angle_max_ = laser_scan->range_max;
		polar_to_cartesian_matrix_angle_increment_ = laser_scan->angle_increment;

		double current_angle = laser_scan->angle_min;
		for (size_t point_pos = 0; point_pos < number_of_scan_points; ++point_pos) {
			polar_to_cartesian_matrix_(0, point_pos) = std::cos(current_angle);
			polar_to_cartesian_matrix_(1, point_pos) = std::sin(current_angle);
			current_angle += laser_scan->angle_increment;
		}

		return true;
	}

	return false;
}


bool LaserScanToPointcloud::integrateLaserScanWithShpericalLinearInterpolation(const sensor_msgs::LaserScanConstPtr& laser_scan) {
	// laser info
	size_t number_of_scan_points = laser_scan->ranges.size();
	size_t number_of_scan_steps = number_of_scan_points - 1;
	ros::Duration scan_duration((double)number_of_scan_steps * (double)laser_scan->time_increment);
	ros::Time scan_start_time = laser_scan->header.stamp;
//	ros::Time scan_end_time = scan_start_time + scan_duration;
	ros::Time scan_middle_time = scan_start_time;
	if (laser_scan->time_increment > 0.0) {
		scan_middle_time += ros::Duration(scan_duration.toSec() / 2.0);
	}

	std::string laser_frame = laser_frame_.empty() ? laser_scan->header.frame_id : laser_frame_;

	// tfs setup
	ros::Time tf_query_time = ((number_of_tf_queries_for_spherical_interpolation_ < 2) || (laser_scan->time_increment <= 0.0)) ? scan_middle_time : scan_start_time;
	tf2::Transform point_transform;
	if (!lookForTransformWithRecovery(point_transform, target_frame_, laser_frame, tf_query_time, tf_lookup_timeout_)) { return false; }


	// projection and transformation setup
	updatePolarToCartesianProjectionMatrix(laser_scan);
	double min_range_cutoff = laser_scan->range_min * min_range_cutoff_percentage_offset_;
	double max_range_cutoff = laser_scan->range_max * max_range_cutoff_percentage_offset_;


	// spherical interpolation setup
	double laser_slice_time_increment_double = ((number_of_tf_queries_for_spherical_interpolation_ < 2) || (laser_scan->time_increment <= 0.0)) ? scan_duration.toSec() : scan_duration.toSec() / (double)(number_of_tf_queries_for_spherical_interpolation_ - 1);
	ros::Duration laser_slice_time_increment(laser_slice_time_increment_double);

	ros::Time past_tf_time = scan_start_time;
	tf2::Vector3 past_tf_translation = point_transform.getOrigin();
	tf2::Quaternion past_tf_rotation = point_transform.getRotation();

	size_t future_tf_number = 1;
	ros::Time future_tf_time = past_tf_time + laser_slice_time_increment;
	tf2::Vector3 future_tf_translation;
	tf2::Quaternion future_tf_rotation;

	bool future_tf_valid = false;
	if ((number_of_tf_queries_for_spherical_interpolation_ > 1) && (laser_scan->time_increment > 0.0)) {
		while (future_tf_number < number_of_tf_queries_for_spherical_interpolation_) {
			future_tf_valid = lookForTransformWithRecovery(future_tf_translation, future_tf_rotation, target_frame_, laser_frame, future_tf_time, tf_lookup_timeout_);
			if (future_tf_valid) { break; } else { ++future_tf_number; future_tf_time += laser_slice_time_increment; }
		}
	}


	// laser scan projection and transformation
	setupPointCloudForNewLaserScan(laser_scan->ranges.size());  // virtual
	ros::Time current_point_time = scan_start_time;
	ros::Duration laser_time_increment(laser_scan->time_increment);
	tf2Scalar current_interpolation_ratio = 0.0;

	for (size_t point_index = 0; point_index < number_of_scan_points; ++point_index) {
		float point_range_value = laser_scan->ranges[point_index];
		if (point_range_value > min_range_cutoff && point_range_value < max_range_cutoff) {
			// project laser scan point in 2D (in the laser frame of reference)
			tf2::Vector3 projected_point(point_range_value * polar_to_cartesian_matrix_(0, point_index), point_range_value * polar_to_cartesian_matrix_(1, point_index), 0);

			// interpolate position and rotation
			if (future_tf_valid) {
				current_interpolation_ratio = (current_point_time - past_tf_time).toSec() / laser_slice_time_increment_double;
				point_transform.getOrigin().setInterpolate3(past_tf_translation, future_tf_translation, current_interpolation_ratio);
				point_transform.setRotation(tf2::slerp(past_tf_rotation, future_tf_rotation, current_interpolation_ratio));
			}

			// transform point to target frame of reference
			tf2::Vector3 transformed_point = point_transform * projected_point;

			if (!remove_invalid_measurements_ ||
				(boost::math::isfinite(transformed_point.x()) && boost::math::isfinite(transformed_point.y()) && boost::math::isfinite(transformed_point.z()))) {
				// copy point to pointcloud
				float intensity = 0;
				if (point_index < laser_scan->intensities.size()) {
					intensity = (float)laser_scan->intensities[point_index];
				}

				addMeasureToPointCloud(transformed_point, intensity);  // virtual
				++number_of_points_in_cloud_;
			}
		}

		if (future_tf_valid) {
			current_point_time += laser_time_increment;
			if (current_point_time > future_tf_time) {
				past_tf_time = future_tf_time;
				past_tf_translation = future_tf_translation;
				past_tf_rotation = future_tf_rotation;

				future_tf_time = past_tf_time + laser_slice_time_increment;
				++future_tf_number;
				while (future_tf_number < number_of_tf_queries_for_spherical_interpolation_) {
					future_tf_valid = lookForTransformWithRecovery(future_tf_translation, future_tf_rotation, target_frame_, laser_frame, future_tf_time, tf_lookup_timeout_);
					if (future_tf_valid) { break; } else { ++future_tf_number; future_tf_time += laser_slice_time_increment; }
				}
			}
		}
	}

	finishLaserScanIntegration(); // virtual
	++number_of_scans_assembled_in_current_pointcloud_;
	return true;
}


bool LaserScanToPointcloud::lookForTransformWithRecovery(tf2::Vector3& translation_out, tf2::Quaternion& rotation_out, const std::string& target_frame, const std::string& source_frame, const ros::Time& time, const ros::Duration& timeout) {
	bool transform_available = tf_collector_.lookForTransform(translation_out, rotation_out, target_frame, source_frame, time, timeout);

	if (!transform_available) { // try to recover using [ sensor_frame -> recovery_frame -> target_frame ]
		if (recovery_frame_.empty()) {
			ROS_WARN_STREAM("Laser assembler couldn't get TF [ " << source_frame << " -> " << target_frame << " ] at time " << time << " with TF timeout of " << timeout.toSec() << " seconds");
			return false;
		}

		// try to update the recovery tf (if fails, uses the last one)
		tf_collector_.lookForTransform(recovery_to_target_frame_transform_, target_frame, recovery_frame_, time, timeout);
		tf2::Transform point_transform;
		transform_available = tf_collector_.lookForTransform(point_transform, recovery_frame_, source_frame, time, timeout);

		if (!transform_available) {
			ROS_WARN_STREAM("Laser assembler couldn't get TF [ " << source_frame << " -> " << recovery_frame_ << " ] at time " << time << " with TF timeout of " << timeout.toSec() << " seconds");
			return false;
		}

		ROS_WARN_STREAM("Recovering from lack of tf between " << source_frame << " and " << target_frame << " using " << recovery_frame_ << " as recovery frame");

		point_transform = recovery_to_target_frame_transform_ * point_transform;
		translation_out = point_transform.getOrigin();
		rotation_out = point_transform.getRotation();
	}

	return true;
}


bool LaserScanToPointcloud::lookForTransformWithRecovery(tf2::Transform& point_transform_out, const std::string& target_frame, const std::string& source_frame, const ros::Time& time, const ros::Duration& timeout) {
	tf2::Vector3 translation_out;
	tf2::Quaternion rotation_out;

	if (lookForTransformWithRecovery(translation_out, rotation_out, target_frame, source_frame, time, timeout)) {
		point_transform_out.setOrigin(translation_out);
		point_transform_out.setRotation(rotation_out);
		return true;
	}

	return false;
}


void LaserScanToPointcloud::setRecoveryFrame(const std::string& recovery_frame, const tf2::Transform& recovery_to_target_frame_transform) {
	recovery_frame_ = recovery_frame; recovery_to_target_frame_transform_ = recovery_to_target_frame_transform;
}
// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>   </LaserScanToPointcloud-functions>  <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<
// =============================================================================  </public-section>   ==========================================================================

// =============================================================================   <protected-section>   =======================================================================
// =============================================================================   </protected-section>  =======================================================================

// =============================================================================   <private-section>   =========================================================================
// =============================================================================   </private-section>  =========================================================================
} /* namespace laserscan_to_pointcloud */
