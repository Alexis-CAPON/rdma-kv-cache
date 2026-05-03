#include "cpp/apps/orchestrator/request_tracker.h"
#include "cpp/common/logger.h"
#include <chrono>
#include <mutex>

void RequestTrackerOrchestrator::track_request(const RequestInfo &request, int client_fd)
{
    std::unique_lock<std::shared_mutex> lock(mutex_);

    TrackedRequest tracked;
    tracked.info = request;
    tracked.status = RequestStatus::PENDING;
    tracked.client_fd = client_fd;

    requests_[request.request_id] = tracked;

    Logger::info("RequestTracker: Started tracking request " + request.request_id +
                 " from client FD " + std::to_string(client_fd));
}

void RequestTrackerOrchestrator::update_status(const std::string &request_id, RequestStatus status)
{
    std::unique_lock<std::shared_mutex> lock(mutex_);

    auto it = requests_.find(request_id);
    if (it == requests_.end())
    {
        Logger::warning("RequestTracker: Cannot update status for unknown request " + request_id);
        return;
    }

    RequestStatus old_status = it->second.status;
    it->second.status = status;

    // Update timestamps based on status transitions
    uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();

    switch (status)
    {
    case RequestStatus::PREFILLING:
        if (it->second.info.timestamp_prefill_start == 0)
        {
            it->second.info.timestamp_prefill_start = now;
        }
        break;
    case RequestStatus::TRANSFERRING:
        if (it->second.info.timestamp_prefill_done == 0)
        {
            it->second.info.timestamp_prefill_done = now;
        }
        break;
    case RequestStatus::DECODING:
        if (it->second.info.timestamp_decode_start == 0)
        {
            it->second.info.timestamp_decode_start = now;
        }
        break;
    case RequestStatus::COMPLETED:
    case RequestStatus::FAILED:
    case RequestStatus::ERROR:
        if (it->second.info.timestamp_decode_done == 0)
        {
            it->second.info.timestamp_decode_done = now;
        }
        break;
    default:
        break;
    }

    Logger::debug("RequestTracker: Request " + request_id + " status: " +
                  std::to_string(static_cast<int>(old_status)) + " -> " +
                  std::to_string(static_cast<int>(status)));
}

void RequestTrackerOrchestrator::mark_prefill_complete(const std::string &request_id)
{
    std::unique_lock<std::shared_mutex> lock(mutex_);

    auto it = requests_.find(request_id);
    if (it == requests_.end())
    {
        Logger::warning("RequestTracker: Cannot mark prefill complete for unknown request " + request_id);
        return;
    }

    uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();

    it->second.info.timestamp_prefill_done = now;
    it->second.status = RequestStatus::TRANSFERRING;

    double latency = calculate_latency_ms(it->second.info.timestamp_prefill_start, now);
    Logger::info("RequestTracker: Request " + request_id + " prefill complete (" +
                 std::to_string(latency) + " ms)");
}

void RequestTrackerOrchestrator::mark_decode_complete(const std::string &request_id,
                                                       const std::string &response)
{
    std::unique_lock<std::shared_mutex> lock(mutex_);

    auto it = requests_.find(request_id);
    if (it == requests_.end())
    {
        Logger::warning("RequestTracker: Cannot mark decode complete for unknown request " + request_id);
        return;
    }

    uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();

    it->second.info.timestamp_decode_done = now;
    it->second.status = RequestStatus::COMPLETED;
    it->second.response = response;

    double decode_latency = calculate_latency_ms(it->second.info.timestamp_decode_start, now);
    double total_latency = calculate_latency_ms(it->second.info.timestamp_created, now);

    Logger::info("RequestTracker: Request " + request_id + " completed (decode: " +
                 std::to_string(decode_latency) + " ms, total: " +
                 std::to_string(total_latency) + " ms)");
}

void RequestTrackerOrchestrator::mark_failed(const std::string &request_id, const std::string &error)
{
    std::unique_lock<std::shared_mutex> lock(mutex_);

    auto it = requests_.find(request_id);
    if (it == requests_.end())
    {
        Logger::warning("RequestTracker: Cannot mark failed for unknown request " + request_id);
        return;
    }

    it->second.status = RequestStatus::FAILED;
    it->second.error = error;

    Logger::error("RequestTracker: Request " + request_id + " failed: " + error);
}

std::optional<RequestInfo> RequestTrackerOrchestrator::get_request(const std::string &request_id) const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);

    auto it = requests_.find(request_id);
    if (it == requests_.end())
    {
        return std::nullopt;
    }

    return it->second.info;
}

std::optional<RequestStatus> RequestTrackerOrchestrator::get_status(const std::string &request_id) const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);

    auto it = requests_.find(request_id);
    if (it == requests_.end())
    {
        return std::nullopt;
    }

    return it->second.status;
}

std::optional<int> RequestTrackerOrchestrator::get_client_fd(const std::string &request_id) const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);

    auto it = requests_.find(request_id);
    if (it == requests_.end())
    {
        return std::nullopt;
    }

    return it->second.client_fd;
}

std::optional<std::string> RequestTrackerOrchestrator::get_response(const std::string &request_id) const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);

    auto it = requests_.find(request_id);
    if (it == requests_.end() || it->second.response.empty())
    {
        return std::nullopt;
    }

    return it->second.response;
}

std::optional<std::string> RequestTrackerOrchestrator::get_error(const std::string &request_id) const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);

    auto it = requests_.find(request_id);
    if (it == requests_.end() || it->second.error.empty())
    {
        return std::nullopt;
    }

    return it->second.error;
}

std::vector<std::string> RequestTrackerOrchestrator::get_timed_out_requests(int timeout_ms) const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);

    std::vector<std::string> timed_out;

    uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();

    for (const auto &[request_id, tracked] : requests_)
    {
        // Only check active requests (not completed/failed)
        if (tracked.status == RequestStatus::COMPLETED ||
            tracked.status == RequestStatus::FAILED ||
            tracked.status == RequestStatus::ERROR)
        {
            continue;
        }

        uint64_t elapsed = now - tracked.info.timestamp_created;
        if (elapsed > static_cast<uint64_t>(timeout_ms))
        {
            timed_out.push_back(request_id);
        }
    }

    return timed_out;
}

void RequestTrackerOrchestrator::remove_request(const std::string &request_id)
{
    std::unique_lock<std::shared_mutex> lock(mutex_);

    auto it = requests_.find(request_id);
    if (it == requests_.end())
    {
        return;
    }

    requests_.erase(it);
    Logger::debug("RequestTracker: Removed request " + request_id + " from tracking");
}

size_t RequestTrackerOrchestrator::cleanup_old_requests(int max_age_ms)
{
    std::unique_lock<std::shared_mutex> lock(mutex_);

    size_t removed_count = 0;

    uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();

    auto it = requests_.begin();
    while (it != requests_.end())
    {
        const auto &tracked = it->second;

        // Only cleanup completed/failed requests
        if (tracked.status != RequestStatus::COMPLETED &&
            tracked.status != RequestStatus::FAILED &&
            tracked.status != RequestStatus::ERROR)
        {
            ++it;
            continue;
        }

        // Use the most recent timestamp available
        uint64_t request_end_time = tracked.info.timestamp_decode_done;
        if (request_end_time == 0)
        {
            request_end_time = tracked.info.timestamp_created;
        }

        uint64_t age = now - request_end_time;
        if (age > static_cast<uint64_t>(max_age_ms))
        {
            it = requests_.erase(it);
            removed_count++;
        }
        else
        {
            ++it;
        }
    }

    if (removed_count > 0)
    {
        Logger::info("RequestTracker: Cleaned up " + std::to_string(removed_count) + " old requests");
    }

    return removed_count;
}

RequestTrackerOrchestrator::Stats RequestTrackerOrchestrator::get_stats() const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);

    Stats stats{};
    stats.total_requests = requests_.size();

    double total_prefill_latency = 0.0;
    double total_decode_latency = 0.0;
    double total_end_to_end_latency = 0.0;

    size_t prefill_latency_count = 0;
    size_t decode_latency_count = 0;
    size_t total_latency_count = 0;

    for (const auto &[request_id, tracked] : requests_)
    {
        // Count by status
        switch (tracked.status)
        {
        case RequestStatus::PENDING:
            stats.pending_requests++;
            break;
        case RequestStatus::PREFILLING:
            stats.prefilling_requests++;
            break;
        case RequestStatus::DECODING:
            stats.decoding_requests++;
            break;
        case RequestStatus::COMPLETED:
            stats.completed_requests++;
            break;
        case RequestStatus::FAILED:
        case RequestStatus::ERROR:
            stats.failed_requests++;
            break;
        default:
            break;
        }

        // Calculate latencies for completed requests
        if (tracked.info.timestamp_prefill_start > 0 && tracked.info.timestamp_prefill_done > 0)
        {
            total_prefill_latency += calculate_latency_ms(
                tracked.info.timestamp_prefill_start,
                tracked.info.timestamp_prefill_done);
            prefill_latency_count++;
        }

        if (tracked.info.timestamp_decode_start > 0 && tracked.info.timestamp_decode_done > 0)
        {
            total_decode_latency += calculate_latency_ms(
                tracked.info.timestamp_decode_start,
                tracked.info.timestamp_decode_done);
            decode_latency_count++;
        }

        if (tracked.info.timestamp_created > 0 && tracked.info.timestamp_decode_done > 0)
        {
            total_end_to_end_latency += calculate_latency_ms(
                tracked.info.timestamp_created,
                tracked.info.timestamp_decode_done);
            total_latency_count++;
        }
    }

    // Calculate averages
    stats.avg_prefill_latency_ms = prefill_latency_count > 0
                                        ? (total_prefill_latency / prefill_latency_count)
                                        : 0.0;

    stats.avg_decode_latency_ms = decode_latency_count > 0
                                       ? (total_decode_latency / decode_latency_count)
                                       : 0.0;

    stats.avg_total_latency_ms = total_latency_count > 0
                                      ? (total_end_to_end_latency / total_latency_count)
                                      : 0.0;

    return stats;
}

double RequestTrackerOrchestrator::calculate_latency_ms(uint64_t start, uint64_t end) const
{
    if (start == 0 || end == 0 || end < start)
    {
        return 0.0;
    }
    return static_cast<double>(end - start);
}
