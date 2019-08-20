#include <sys/stat.h> 
#include <fcntl.h>
#include <cmath>
#include "SendSideBandwidthEstimation.h"

constexpr uint64_t kStartupDuration		= 15E5;		// 1.5 s
constexpr uint64_t kMonitorDuration		= 250E3;	// 250ms
constexpr uint64_t kMonitorTimeout		= 750E3;	// 750ms
constexpr uint64_t kMinRate			= 128E3;	// 128kbps
constexpr uint64_t kMaxRate			= 100E6;	// 100mbps
constexpr uint64_t kMinRateChangeBps		= 4000;
constexpr uint64_t kConversionFactor		= 2;

// Utility function parameters.
constexpr double kDelayGradientCoefficient = 0.005;
constexpr double kLossCoefficient = 10;
constexpr double kThroughputPower = 0.9;


bool MonitorInterval::SentPacket(uint64_t sent, uint64_t size)
{
	//Check it is from this interval
	if (sent<start || sent>GetEndTime())
		//Skip
		return false; //Error("-MonitorInterval::SentPacket() Skip %llu [start:%llu,end:%llu]\n",sent,start,GetEndTime());
	//Check first
	if (firstSent == std::numeric_limits<uint64_t>::max())
		firstSent = sent;
	//Set last
	lastSent = sent;
	//Append size
	accumulatedSentSize += size;
	//One more
	totalSentPackets++;
	
	//Log("-MonitorInterval::SentPacket() adding %llu size=%llu [start:%llu,end:%llu,acu=%llu]\n",sent,size,start,GetEndTime(),accumulatedSentSize);
	//Accepted
	return true;
}

bool MonitorInterval::Feedback(uint64_t sent, uint64_t recv, uint64_t size, int64_t delta)
{
	//Check it is from this interval 
	if (sent<start)
		//Skip packet
		return false; //Error("-MonitorInterval::Feedback() Skip %llu [start:%llu,end:%llu]\n",sent,start,GetEndTime());
	// Here we assume that if some packets are reordered with packets sent
	// after the end of the monitor interval, then they are lost. (Otherwise
	// it is not clear how long should we wait for packets feedback to arrive).
	if (sent > GetEndTime())
	{
		//Completed
		feedbackCollectionDone = true;
		//Skip
		return false; //Error("-MonitorInterval::Feedback() done, skipping %llu [start:%llu,end:%llu]\n",sent,start,GetEndTime());
	}

	//One more
	totalFeedbackedPackets++;
	
	//Check recv time
	if (recv!=std::numeric_limits<uint64_t>::max())
	{
		//Check first
		if (firstRecv == std::numeric_limits<uint64_t>::max())
			//Its first
			firstRecv = recv;
		//Set last
		lastRecv = recv;
		//Increase accumulated received sie
		accumulatedReceivedSize += size;
		//Store deltas
		deltas.emplace_back(sent,delta);
	} else {
		//Increase number of packets losts
		lostPackets ++;
	}
	
	//Log("-MonitorInterval::Feedback() %llu size=%llu [start:%llu,end:%llu,acu=%llu]\n",sent,size,start,GetEndTime(),accumulatedReceivedSize);
	
	//Accepted
	return true;
}

uint64_t MonitorInterval::GetSentBitrate() const
{
	return lastSent!=firstSent ? (accumulatedSentSize * 8E6) / (lastSent - firstSent) : 0; 
}

uint64_t MonitorInterval::GetReceivedBitrate() const
{
	return lastRecv!=firstRecv ? (accumulatedReceivedSize * 8E6) / (lastRecv - firstRecv) : 0; 
}

double MonitorInterval::GetLossRate() const
{
	return totalFeedbackedPackets ? static_cast<double> (lostPackets) / totalFeedbackedPackets : 0;
}


// For the formula used in computations see formula for "slope" in the second method:
// https://www.johndcook.com/blog/2008/10/20/comparing-two-ways-to-fit-a-line-to-data/
double MonitorInterval::ComputeDelayGradient() const
{
	double timeSum = 0;
	double delaySum = 0;
	for (const auto& delta : deltas)
	{
		double timeDelta = delta.first;
		double delay = delta.second;
		timeSum += timeDelta;
		delaySum += delay;
	}
	double squaredScaledTimeDeltaSum = 0;
	double scaledTimeDeltaDelay = 0;
	for (const auto& delta : deltas)
	{
		double timeDelta = delta.first;
		double delay = delta.second;
		double scaledTimeDelta = timeDelta - timeSum / deltas.size();
		squaredScaledTimeDeltaSum += scaledTimeDelta * scaledTimeDelta;
		scaledTimeDeltaDelay += scaledTimeDelta * delay;
	}
	return squaredScaledTimeDeltaSum ? scaledTimeDeltaDelay / squaredScaledTimeDeltaSum : 0;
}

double MonitorInterval::ComputeVivaceUtilityFunction() const 
{
	// Get functio inputs
	double bitrate		= GetSentBitrate();
	double lossrate		= GetLossRate();
	double delayGradient	= ComputeDelayGradient();
	//Calculate the utility
	return std::pow(bitrate, kThroughputPower) - (kDelayGradientCoefficient * delayGradient * bitrate) - (kLossCoefficient * lossrate * bitrate);
}

void MonitorInterval::Dump() const
{
	Log("[MonitorInterval from=%llu to=%llu duration=%llu target=%lubps sent=%llubps recv=%llubps firstSent=%llu lastSent=%llu firstRecv=%llu lastRecv=%llu sentSize=%llu recvSize=%llu totalSent=%lu totalFeedbacked=%lu lost=%lu done=%d/]\n",
		start,
		GetEndTime(),
		duration,
		target,
		GetSentBitrate(),
		GetReceivedBitrate(),
		firstSent,
		lastSent,
		firstRecv,
		lastRecv,
		accumulatedSentSize,
		accumulatedReceivedSize,
		totalSentPackets,
		totalFeedbackedPackets,
		lostPackets,
		feedbackCollectionDone);
}

SendSideBandwidthEstimation::SendSideBandwidthEstimation()
{
}

SendSideBandwidthEstimation::~SendSideBandwidthEstimation()
{
	//If  dumping
	if (fd!=FD_INVALID)
		//Close file
		close(fd);
}
	
void SendSideBandwidthEstimation::SentPacket(const PacketStats::shared& stats)
{
	
	//Check first packet sent time
	if (!firstSent)
	{
		//Set first time
		firstSent = stats->time;
		//Create initial interval
		monitorIntervals.emplace_back(0,kStartupDuration);
	}
	//Get sent time
	uint64_t sentTime = stats->time-firstSent;
	
	//Log("sent #%u sent:%.8lu\n",stats->transportWideSeqNum,sentTime);
	
	//For all intervals
	for (auto& interval : monitorIntervals)
		//Pas it
		interval.SentPacket(sentTime, stats->size);
	
	//Check if last interval has already expired 
	if (sentTime > (monitorIntervals.back().GetEndTime() + rtt + kMonitorTimeout))
	{
		//Calculate new estimation
		EstimateBandwidthRate();
		//ReCreate new intervals again
		CreateIntervals(sentTime);
	}
	
	//Add to history map
	transportWideSentPacketsStats[stats->transportWideSeqNum] = stats;
	//Protect against missfing feedbacks, remove too old lost packets
	auto it = transportWideSentPacketsStats.begin();
	//If there are no intervals for them
	while(it!=transportWideSentPacketsStats.end() && (stats->time-firstSent)<monitorIntervals.front().GetStartTime())
		//Erase it and move iterator
		it = transportWideSentPacketsStats.erase(it);

}

void SendSideBandwidthEstimation::ReceivedFeedback(uint8_t feedbackNum, const std::map<uint32_t,uint64_t>& packets, uint64_t when)
{

	//Check we have packets
	if (packets.empty())
		//Skip
		return;
	
	//For each packet
	for (const auto& feedback : packets)
	{
		//Get feedback data
		auto transportSeqNum	= feedback.first;
		auto receivedTime	= feedback.second; 
		
		//Get packets stats
		auto it = transportWideSentPacketsStats.find(transportSeqNum);
		//If found
		if (it!=transportWideSentPacketsStats.end())
		{
			//Get stats
			const auto& stat = it->second;
			//Get sent time
			const auto sentTime = stat->time;
			
			//Check first feedback received
			if (!firstRecv)
			{
				prevSent = sentTime;
				prevRecv = firstRecv = receivedTime;
			}
			
			//Correc ts
			auto fb   = when - firstSent;
			auto sent = sentTime - firstSent;
			auto recv = receivedTime ? receivedTime - firstRecv : 0;
			//Get deltas
			auto deltaSent = sent - prevSent;
			auto deltaRecv = receivedTime ? recv - prevRecv : 0;
			auto delta = receivedTime ? deltaRecv - deltaSent : 0;
			
			//Dump stats
			//Log("recv #%u sent:%.8lu (+%.6lu) recv:%.8lu (+%.6lu) delta:%.6ld fb:%u, size:%u, bwe:%lu\n",transportSeqNum,sent,deltaSent,recv,deltaRecv,delta,feedbackNum, stat->size, bandwidthEstimation);
			
			bool completed = true;
			//For all intervals
			for (auto& interval : monitorIntervals)
			{
				//Pas it
				interval.Feedback(sent,receivedTime? recv : std::numeric_limits<uint64_t>::max(),stat->size,delta);
				//Check if they are completed
				completed &= interval.IsFeedbackCollectionDone();
			}
			
			//If all intervals are completed now
			if (completed)
			{
				//Calculate new estimation
				EstimateBandwidthRate();
				//Create new intervals
				CreateIntervals(sent);
			}

			//If dumping to file
			if (fd)
			{
				char msg[1024];
				//Create log
				int len = snprintf(msg,1024,"%.8lu|%u|%u|%u|%.8lu|%.8lu|%.6lu|%.6lu|%ld|%lu|%u|%d|%d|%d\n",fb,transportSeqNum,feedbackNum, stat->size,sent,recv,deltaSent,deltaRecv,delta,bandwidthEstimation,rtt,stat->mark,stat->rtx,stat->probing);
				//Write it
				write(fd,msg,len);
			}

			//Check if it was not lost
			if (receivedTime)
			{
				//Update last received time
				lastRecv = receivedTime;
				//And previous
				prevSent = sent;
				prevRecv = recv;
			}	

			//Erase it
			transportWideSentPacketsStats.erase(it);
		}
	}
}

void SendSideBandwidthEstimation::UpdateRTT(uint32_t rtt)
{
	//Store rtt
	this->rtt = rtt;
	//Smooth
	//rttTracker.UpdateRtt(rtt);
}

uint32_t SendSideBandwidthEstimation::GetEstimatedBitrate() const
{
	return bandwidthEstimation;
}

uint32_t SendSideBandwidthEstimation::GetTargetBitrate() const
{
	//Find current interval
	for (const auto& interval : monitorIntervals)
		//If is not completed yet
		if (!interval.IsFeedbackCollectionDone())
			//This is the current target
			return interval.GetTargetBitrate();
	return bandwidthEstimation;
}


void SendSideBandwidthEstimation::CreateIntervals(uint64_t time)
{
	//Log("-SendSideBandwidthEstimation::CreateIntervals() [time:%llu]\n",time);
	
	//Drop previous ones
	monitorIntervals.clear();
	
	//Ramdomize if we have to increase or decrease rate first
	int64_t sign = 2 * (std::rand() % 2) - 1;
	
	//Calculate step
	uint64_t step = std::max(static_cast<uint64_t>(bandwidthEstimation * 0.1),kMinRateChangeBps);

	//Calculate probing btirates for monitors
	uint64_t monitorIntervalsBitrates[2] = {
		std::min(bandwidthEstimation  + sign * step, kMaxRate),
		std::max(bandwidthEstimation  - sign * step, kMinRate)
	};
	
	//Create two consecutive monitoring intervals
	monitorIntervals.emplace_back(monitorIntervalsBitrates[0], time, kMonitorDuration);
	monitorIntervals.emplace_back(monitorIntervalsBitrates[1], time + kMonitorDuration, kMonitorDuration);
	
	//For all packets with still no feedback
	for (const auto& entry : transportWideSentPacketsStats)
	{
		//Get sent time
		uint64_t sentTime = entry.second->time - firstSent;
		uint64_t size = entry.second->size;
		//For all new intervals
		for (auto& interval : monitorIntervals)
			//Pas it
			interval.SentPacket(sentTime, size);
	}
	
}

void SendSideBandwidthEstimation::EstimateBandwidthRate()
{
	//Log("-SendSideBandwidthEstimation::EstimateBandwidthRate()\n");
	
	if (monitorIntervals.empty())
		return;
	
//	for (const auto& interval : monitorIntervals)
//		interval.Dump();
	
	//If startup phase completed
	if (monitorIntervals.size()==1)
	{
		//Calculate initial value based on received data so far
		availableRate = bandwidthEstimation = monitorIntervals[0].GetReceivedBitrate();
		//Log it
		//Log("-SendSideBandwidthEstimation::EstimateBandwidthRate() [estimate:%llubps]\n",bandwidthEstimation);
		//Set new extimate
		if (listener)
			listener->onTargetBitrateRequested(availableRate);
		//Done
		return;
	}
	//Get deltas gradients
	double delta0	= monitorIntervals[0].ComputeDelayGradient();
	double delta1	= monitorIntervals[1].ComputeDelayGradient();
	//Calcualte utilities for each interval
	double utility0 = monitorIntervals[0].ComputeVivaceUtilityFunction();
	double utility1 = monitorIntervals[1].ComputeVivaceUtilityFunction();
	//Get actual sent rate
	double bitrate0 = monitorIntervals[0].GetSentBitrate();
	double bitrate1 = monitorIntervals[1].GetSentBitrate();
	//Get actual target bitrate
	uint64_t targetBitrate = bitrate0 && bitrate1 ? (bitrate0 + bitrate1) / 2 : bitrate0 + bitrate1;
	
	//The utility gradient
	double gradient = (utility0 - utility1) / (bitrate0 - bitrate1);
	
	//Get previous state change
	auto prevState = state;

	//Check if we have sent much more than expected
	if (targetBitrate>std::max(monitorIntervals[0].GetTargetBitrate(),monitorIntervals[1].GetReceivedBitrate()))
		//We are overshooting
		state = ChangeState::OverShoot;
	else
		//Get state from gradient
		state = gradient ? ChangeState::Increase : ChangeState::Decrease;

	//Set bumber of consecutive chantes
	if (prevState == state)
		consecutiveChanges++;
	else
		consecutiveChanges = 0;
	
	//If not overshooting
	if (state!=ChangeState::OverShoot)
	{
		//Initial conversion factor
		double confidenceAmplifier = std::log(consecutiveChanges+1);
		//Get rate change
		uint64_t rateChange = gradient * confidenceAmplifier * kConversionFactor;
		//Set it
		bandwidthEstimation = targetBitrate + rateChange;
	
	} else {
		//Only use recevied bitrate
		bandwidthEstimation = std::min(monitorIntervals[0].GetReceivedBitrate(),monitorIntervals[1].GetReceivedBitrate());
	}
	
	//Adjust max/min rates
	bandwidthEstimation = std::min(std::max(bandwidthEstimation,kMinRate),kMaxRate);
	
	//Get loss rate
	double lossRate = std::max(monitorIntervals[0].GetLossRate(),monitorIntervals[1].GetLossRate());
	//Corrected rate
	availableRate = bandwidthEstimation * ( 1 - lossRate);
	
//	Log("-SendSideBandwidthEstimation::EstimateBandwidthRate() [estimate:%llubps,available:%llubps,target:%llubps,bitrate0:%llubps,bitrate1:%llubps,utility0:%f,utility1:%f,gradient:%f,state:%d\n",
//		bandwidthEstimation,
//		targetBitrate,
//		availableRate,
//		static_cast<uint64_t>(bitrate0),
//		static_cast<uint64_t>(bitrate1),
//		utility0,
//		utility1,
//		gradient,
//		state
//	);
	
	//Set new extimate
	if (listener)
		//Call listener	
		listener->onTargetBitrateRequested(availableRate);
}

int SendSideBandwidthEstimation::Dump(const char* filename) 
{
	//If already dumping
	if (fd!=FD_INVALID)
		//Error
		return 0;
	
	Log("-SendSideBandwidthEstimation::Dump [\"%s\"]\n",filename);
	
	//Open file
	if ((fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0600))<0)
		//Error
		return false; //Error("Could not open file [err:%d]\n",errno);

	//Done
	return 1;
}
