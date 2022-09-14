#include <memory>
#include <mutex>

#ifdef _WIN32
#include <share.h>
#endif

#include "Common/StringUtil.h"
#include "Common/Logging/Log.h"
#include "Common/FileUtil.h"
#include "Common/CommonPaths.h"
#include "Core/Core.h"
#include "Core/HW/EXI_DeviceSlippi.h"
#include "Core/NetPlayClient.h"
#include "Core/State.h"
#include "SlippiPlayback.h"
#include <VideoCommon/OnScreenDisplay.h>

#define FRAME_INTERVAL 900
#define SLEEP_TIME_MS 8

std::unique_ptr<SlippiPlaybackStatus> g_playbackStatus;
extern std::unique_ptr<SlippiReplayComm> g_replayComm;

static std::mutex mtx;
static std::mutex seekMtx;
static std::mutex diffMtx;
static std::unique_lock<std::mutex> processingLock(diffMtx);
static std::condition_variable condVar;
static std::condition_variable cv_waitingForTargetFrame;
static std::condition_variable cv_processingDiff;
static std::atomic<int> numDiffsProcessing(0);

s32 emod(s32 a, s32 b)
{
	assert(b != 0);
	int r = a % b;
	return r >= 0 ? r : r + std::abs(b);
}

std::string processDiff(std::vector<u8> iState, std::vector<u8> cState)
{
	INFO_LOG(SLIPPI, "Processing diff");
	numDiffsProcessing += 1;
	cv_processingDiff.notify_one();
	std::string diff = std::string();
	open_vcdiff::VCDiffEncoder encoder((char *)iState.data(), iState.size());
	encoder.Encode((char *)cState.data(), cState.size(), &diff);

	INFO_LOG(SLIPPI, "done processing");
	numDiffsProcessing -= 1;
	cv_processingDiff.notify_one();
	return diff;
}

SlippiPlaybackStatus::SlippiPlaybackStatus()
{
	shouldJumpBack = false;
	shouldJumpForward = false;
	inSlippiPlayback = false;
	shouldRunThreads = false;
	isHardFFW = false;
	isSoftFFW = false;
	lastFFWFrame = INT_MIN;
	currentPlaybackFrame = INT_MIN;
	targetFrameNum = INT_MAX;
	latestFrame = Slippi::GAME_FIRST_FRAME;
	prevOCEnable = SConfig::GetInstance().m_OCEnable;
	prevOCFactor = SConfig::GetInstance().m_OCFactor;

	// Only generate these if this is a playback configuration. Should this class get initialized at all?
	#ifdef IS_PLAYBACK
	generateDenylist();
	generateLegacyCodelist();
	#endif
}

void SlippiPlaybackStatus::startThreads()
{
	shouldRunThreads = true;

	if (!m_savestateThread.joinable())
	{
		m_savestateThread = std::thread(&SlippiPlaybackStatus::SavestateThread, this);
	}
	
	if (!m_seekThread.joinable())
	{
		m_seekThread = std::thread(&SlippiPlaybackStatus::SeekThread, this);
	}
}

void SlippiPlaybackStatus::prepareSlippiPlayback(s32 &frameIndex)
{
	// block if there's too many diffs being processed
	while (shouldRunThreads && numDiffsProcessing > 3)
	{
		INFO_LOG(SLIPPI, "Processing too many diffs, blocking main process");
		cv_processingDiff.wait(processingLock);
	}

	// Unblock thread to save a state every interval
	if (shouldRunThreads && ((currentPlaybackFrame + 122) % FRAME_INTERVAL == 0))
		condVar.notify_one();

	if (SConfig::GetInstance().m_slippiEnableFrameIndex)
	{
		std::stringstream frameDisplay;
		frameDisplay << "Frame: " + std::to_string(frameIndex);
		INFO_LOG(SLIPPI_ONLINE, "Replay Frame: %d", frameIndex);

		OSD::AddTypedMessage(OSD::MessageType::FrameIndex, frameDisplay.str(), 1000, OSD::Color::CYAN);
	}

	// TODO: figure out why sometimes playback frame increments past targetFrameNum
	if (inSlippiPlayback && frameIndex >= targetFrameNum)
	{
		if (targetFrameNum < currentPlaybackFrame)
		{
			// Since playback logic only goes up in currentPlaybackFrame now due to handling rollback
			// playback, we need to rewind the currentPlaybackFrame here instead such that the playback
			// cursor will show up in the correct place
			currentPlaybackFrame = targetFrameNum;
		}

		if (currentPlaybackFrame > targetFrameNum)
		{
			INFO_LOG(SLIPPI, "Reached frame %d. Target was %d. Unblocking", currentPlaybackFrame, targetFrameNum);
		}
		cv_waitingForTargetFrame.notify_one();
	}
}

void SlippiPlaybackStatus::resetPlayback()
{
	if (shouldRunThreads)
	{
		shouldRunThreads = false;

		if (m_savestateThread.joinable())
			m_savestateThread.detach();

		if (m_seekThread.joinable())
			m_seekThread.detach();

		condVar.notify_one(); // Will allow thread to kill itself
		futureDiffs.clear();
		futureDiffs.rehash(0);
	}

	shouldJumpBack = false;
	shouldJumpForward = false;
	isHardFFW = false;
	isSoftFFW = false;
	targetFrameNum = INT_MAX;
	inSlippiPlayback = false;
}

void SlippiPlaybackStatus::processInitialState(std::vector<u8> &iState)
{
	INFO_LOG(SLIPPI, "saving iState");
	State::SaveToBuffer(iState);
};

void SlippiPlaybackStatus::SavestateThread()
{
	Common::SetCurrentThreadName("Savestate thread");
	std::unique_lock<std::mutex> intervalLock(mtx);

	INFO_LOG(SLIPPI, "Entering savestate thread");

	while (shouldRunThreads)
	{
		// Wait to hit one of the intervals
		// Possible while rewinding that we hit this wait again.
		while (shouldRunThreads && (currentPlaybackFrame - Slippi::PLAYBACK_FIRST_SAVE) % FRAME_INTERVAL != 0)
			condVar.wait(intervalLock);

		if (!shouldRunThreads)
			break;

		s32 fixedFrameNumber = currentPlaybackFrame;
		if (fixedFrameNumber == INT_MAX)
			continue;

		bool isStartFrame = fixedFrameNumber == Slippi::PLAYBACK_FIRST_SAVE;
		bool hasStateBeenProcessed = futureDiffs.count(fixedFrameNumber) > 0;

		if (!inSlippiPlayback && isStartFrame)
		{
			processInitialState(iState);
			inSlippiPlayback = true;
		}
		else if (SConfig::GetInstance().m_InterfaceSeekbar && !SConfig::GetInstance().m_CLIHideSeekbar &&
		         !hasStateBeenProcessed && !isStartFrame)
		{
			INFO_LOG(SLIPPI, "saving diff at frame: %d", fixedFrameNumber);
			State::SaveToBuffer(cState);

			futureDiffs[fixedFrameNumber] = std::async(processDiff, iState, cState);
		}
		Common::SleepCurrentThread(SLEEP_TIME_MS);
	}

	INFO_LOG(SLIPPI, "Exiting savestate thread");
}

void SlippiPlaybackStatus::SeekThread()
{
	Common::SetCurrentThreadName("Seek thread");
	std::unique_lock<std::mutex> seekLock(seekMtx);

	INFO_LOG(SLIPPI, "Entering seek thread");

	while (shouldRunThreads)
	{
		bool shouldSeek = inSlippiPlayback && (shouldJumpBack || shouldJumpForward || targetFrameNum != INT_MAX);

		if (shouldSeek)
		{
			auto replayCommSettings = g_replayComm->getSettings();
			if (replayCommSettings.mode == "queue")
				updateWatchSettingsStartEnd();

			bool paused = (Core::GetState() == Core::CORE_PAUSE);
			Core::SetState(Core::CORE_PAUSE);

			u32 jumpInterval = 300; // 5 seconds;

			if (shouldJumpForward)
				targetFrameNum = currentPlaybackFrame + jumpInterval;

			if (shouldJumpBack)
				targetFrameNum = currentPlaybackFrame - jumpInterval;

			// Handle edgecases for trying to seek before start or past end of game
			if (targetFrameNum < Slippi::PLAYBACK_FIRST_SAVE)
				targetFrameNum = Slippi::PLAYBACK_FIRST_SAVE;

			if (targetFrameNum > latestFrame)
			{
				targetFrameNum = latestFrame;
			}

			s32 closestStateFrame = targetFrameNum - emod(targetFrameNum - Slippi::PLAYBACK_FIRST_SAVE, FRAME_INTERVAL);

			// Somtimes prepareSlippiPlayback sets currentPlaybackFrame = targetFrameNum so check if target is <=
			bool isLoadingStateOptimal =
			    targetFrameNum <= currentPlaybackFrame || closestStateFrame > currentPlaybackFrame;

			if (isLoadingStateOptimal)
			{
				if (closestStateFrame <= Slippi::PLAYBACK_FIRST_SAVE)
				{
					State::LoadFromBuffer(iState);
				}
				else
				{
					// If this diff has been processed, load it
					if (futureDiffs.count(closestStateFrame) > 0)
					{
						loadState(closestStateFrame);
					}
					else if (targetFrameNum < currentPlaybackFrame)
					{
						s32 closestActualStateFrame = closestStateFrame - FRAME_INTERVAL;
						while (closestActualStateFrame > Slippi::PLAYBACK_FIRST_SAVE &&
						       futureDiffs.count(closestActualStateFrame) == 0)
							closestActualStateFrame -= FRAME_INTERVAL;
						loadState(closestActualStateFrame);
					}
					else if (targetFrameNum > currentPlaybackFrame)
					{
						s32 closestActualStateFrame = closestStateFrame - FRAME_INTERVAL;
						while (closestActualStateFrame > currentPlaybackFrame &&
						       futureDiffs.count(closestActualStateFrame) == 0)
							closestActualStateFrame -= FRAME_INTERVAL;

						// only load a savestate if we find one past our current frame since we are seeking forwards
						if (closestActualStateFrame > currentPlaybackFrame)
							loadState(closestActualStateFrame);
					}
				}
			}

			// Fastforward until we get to the frame we want
			if (targetFrameNum != closestStateFrame && targetFrameNum != latestFrame)
			{
				setHardFFW(true);

				Core::SetState(Core::CORE_RUN);
				cv_waitingForTargetFrame.wait(seekLock);
				Core::SetState(Core::CORE_PAUSE);

				setHardFFW(false);
			}

			if (!paused)
				Core::SetState(Core::CORE_RUN);

			shouldJumpBack = false;
			shouldJumpForward = false;
			targetFrameNum = INT_MAX;
		}

		Common::SleepCurrentThread(SLEEP_TIME_MS);
	}

	INFO_LOG(SLIPPI, "Exit seek thread");
}

// Set isHardFFW and update OC settings to speed up the FFW
void SlippiPlaybackStatus::setHardFFW(bool enable)
{
	isHardFFW = enable;
	if (isHardFFW)
	{
		SConfig::GetInstance().m_OCEnable = true;
		SConfig::GetInstance().m_OCFactor = 4.0f;
	}
	else
	{
		SConfig::GetInstance().m_OCFactor = prevOCFactor;
		SConfig::GetInstance().m_OCEnable = prevOCEnable;
	}
}

void SlippiPlaybackStatus::loadState(s32 closestStateFrame)
{
	if (closestStateFrame == Slippi::PLAYBACK_FIRST_SAVE)
		State::LoadFromBuffer(iState);
	else
	{
		std::string stateString;
		decoder.Decode((char *)iState.data(), iState.size(), futureDiffs[closestStateFrame].get(), &stateString);
		std::vector<u8> stateToLoad(stateString.begin(), stateString.end());
		State::LoadFromBuffer(stateToLoad);
	}
}

bool SlippiPlaybackStatus::shouldFFWFrame(int32_t frameIndex) const
{
	if (!isSoftFFW && !isHardFFW)
	{
		// If no FFW at all, don't FFW this frame
		return false;
	}

	if (isHardFFW)
	{
		// For a hard FFW, always FFW until it's turned off
		return true;
	}

	// Here we have a soft FFW, we only want to turn on FFW for single frames once
	// every X frames to FFW in a more smooth manner
	return (frameIndex - lastFFWFrame) >= 15;
}

void SlippiPlaybackStatus::updateWatchSettingsStartEnd()
{
	int startFrame = g_replayComm->current.startFrame;
	int endFrame = g_replayComm->current.endFrame;
	if (startFrame != Slippi::GAME_FIRST_FRAME || endFrame != INT_MAX)
	{
		if (g_playbackStatus->targetFrameNum < startFrame)
			g_replayComm->current.startFrame = g_playbackStatus->targetFrameNum;
		if (g_playbackStatus->targetFrameNum > endFrame)
			g_replayComm->current.endFrame = INT_MAX;
	}
}

std::unordered_map<u32, bool> SlippiPlaybackStatus::getDenylist()
{
	return denylist;
}

std::vector<u8> SlippiPlaybackStatus::getLegacyCodelist()
{
	return legacyCodelist;
}

inline std::string readString(json obj, std::string key)
{
	auto item = obj.find(key);
	if (item == obj.end() || item.value().is_null())
	{
		return "";
	}

	return obj[key];
}

int getOrderNumFromFileName(std::string name) 
{
	// Extract last value after a dash, then try to parse it into a number. This is the
	// number we will sort by. If there is no number present, the number used is 0.
	std::string last;
	std::istringstream f(name);
	std::string s;
	while (std::getline(f, s, '-'))
	{
		last = s;
	}

	int num;
	if (!TryParse(last, &num))
	{
		num = 0;
	}

	return num;
}

// Compares two intervals according to starting times.
bool compareInjectionList(File::FSTEntry i1, File::FSTEntry i2)
{
	return getOrderNumFromFileName(i1.virtualName) < getOrderNumFromFileName(i2.virtualName);
}

void SlippiPlaybackStatus::generateDenylist()
{
	// We start by populating the denylist with old injections that are not longer used but need
	// to be included for backward compatibility reasons.
	// It also includes some common codes that are not in our codebase
	denylist = {
	    // Backward compatibility
	    // Post 3.4.0: Recording/FlushFrameBuffer.asm
	    {0x802fef88, true},
	    // Post 3.4.0: Recording/SendGamePostFrame.asm
	    {0x8006c5d8, true},
	    // Post 3.7.0: Recording/SendGameEnd.asm
	    {0x8016d30c, true},

	    // Common codes not in our codebase
	    // HUD Transparency v1.1 (https://smashboards.com/threads/transparent-hud-v1-1.508509/)
	    {0x802f6690, true},
	    // Smaller "Ready, GO!" (https://smashboards.com/threads/smaller-ready-go.509740/)
	    {0x802F71E0, true},
	    // Yellow During IASA (https://smashboards.com/threads/color-overlays-for-iasa-frames.401474/post-19120928)
	    {0x80071960, true},
	    // Turn Green When Actionable (https://blippi.gg/codes)
	    {0x800CC818, true},
	    {0x8008A478, true},
	};

	// Next we parse through the injection lists files to exclude all of our injections that don't affect gameplay
	std::string injections_path = File::GetSysDirectory() + DIR_SEP + "Slippi" + DIR_SEP + "InjectionLists";
	auto entries = File::ScanDirectoryTree(injections_path, false);
	auto children = entries.children;

	// First sort by the file names so later lists take precedence
	std::sort(children.begin(), children.end(), compareInjectionList);

	for (auto &entry : children)
	{
		if (entry.isDirectory)
			continue;

		WARN_LOG(SLIPPI, "Injection List checking: %s. %s", entry.physicalName.c_str(), entry.virtualName.c_str());

		std::string contents;
		File::ReadFileToString(entry.physicalName, contents);
		auto res = json::parse(contents, nullptr, false);
		if (res.is_discarded() || !res.is_object())
		{
			ERROR_LOG(SLIPPI, "Injection list file %s is not properly formatted.", entry.physicalName.c_str());
			continue;
		}

		auto list = res["Details"];
		if (list.is_discarded() || !list.is_array())
		{
			ERROR_LOG(SLIPPI, "Injection list file %s is not properly formatted.", entry.physicalName.c_str());
			continue;
		}

		// Go through all the injections
		for (auto &injection : list)
		{
			if (injection.is_discarded() || !injection.is_object())
			{
				ERROR_LOG(SLIPPI, "Injection entry in list file %s is not properly formatted.",
				          entry.physicalName.c_str());
				continue;
			}

			// Check if tags indicate that this code affects gameplay, if so, do not put it on the denylist
			auto tags = readString(injection, "Tags");
			bool shouldDeny = tags.find("[affects-gameplay]") == std::string::npos;

			// Add injection to denylist
			u32 address;
			auto addressStr = readString(injection, "InjectionAddress");
			if (!AsciiToHex(addressStr, address))
			{
				ERROR_LOG(SLIPPI, "Injection list file %s: Could not parse address: %s", entry.physicalName.c_str(),
				          addressStr.c_str());
				continue;
			}
			denylist[address] = shouldDeny;
			// INFO_LOG(SLIPPI, "New denylist entry: %08X", address);
		}
	}

	NOTICE_LOG(SLIPPI, "Denylist populated with length: %d", denylist.size());
}

void SlippiPlaybackStatus::generateLegacyCodelist() {
	legacyCodelist = {
	    0xC2, 0x0C, 0x9A, 0x44, 0x00, 0x00, 0x00, 0x2F, // #External/UCF + Arduino Toggle UI/UCF/UCF 0.74
	                                                    // Dashback - Check for Toggle.asm
	    0xD0, 0x1F, 0x00, 0x2C, 0x88, 0x9F, 0x06, 0x18, 0x38, 0x62, 0xF2, 0x28, 0x7C, 0x63, 0x20, 0xAE, 0x2C, 0x03,
	    0x00, 0x01, 0x41, 0x82, 0x00, 0x14, 0x38, 0x62, 0xF2, 0x2C, 0x7C, 0x63, 0x20, 0xAE, 0x2C, 0x03, 0x00, 0x01,
	    0x40, 0x82, 0x01, 0x50, 0x7C, 0x08, 0x02, 0xA6, 0x90, 0x01, 0x00, 0x04, 0x94, 0x21, 0xFF, 0x50, 0xBE, 0x81,
	    0x00, 0x08, 0x48, 0x00, 0x01, 0x21, 0x7F, 0xC8, 0x02, 0xA6, 0xC0, 0x3F, 0x08, 0x94, 0xC0, 0x5E, 0x00, 0x00,
	    0xFC, 0x01, 0x10, 0x40, 0x40, 0x82, 0x01, 0x18, 0x80, 0x8D, 0xAE, 0xB4, 0xC0, 0x3F, 0x06, 0x20, 0xFC, 0x20,
	    0x0A, 0x10, 0xC0, 0x44, 0x00, 0x3C, 0xFC, 0x01, 0x10, 0x40, 0x41, 0x80, 0x01, 0x00, 0x88, 0x7F, 0x06, 0x70,
	    0x2C, 0x03, 0x00, 0x02, 0x40, 0x80, 0x00, 0xF4, 0x88, 0x7F, 0x22, 0x1F, 0x54, 0x60, 0x07, 0x39, 0x40, 0x82,
	    0x00, 0xE8, 0x3C, 0x60, 0x80, 0x4C, 0x60, 0x63, 0x1F, 0x78, 0x8B, 0xA3, 0x00, 0x01, 0x38, 0x7D, 0xFF, 0xFE,
	    0x88, 0x9F, 0x06, 0x18, 0x48, 0x00, 0x00, 0x8D, 0x7C, 0x7C, 0x1B, 0x78, 0x7F, 0xA3, 0xEB, 0x78, 0x88, 0x9F,
	    0x06, 0x18, 0x48, 0x00, 0x00, 0x7D, 0x7C, 0x7C, 0x18, 0x50, 0x7C, 0x63, 0x19, 0xD6, 0x2C, 0x03, 0x15, 0xF9,
	    0x40, 0x81, 0x00, 0xB0, 0x38, 0x00, 0x00, 0x01, 0x90, 0x1F, 0x23, 0x58, 0x90, 0x1F, 0x23, 0x40, 0x80, 0x9F,
	    0x00, 0x04, 0x2C, 0x04, 0x00, 0x0A, 0x40, 0xA2, 0x00, 0x98, 0x88, 0x7F, 0x00, 0x0C, 0x38, 0x80, 0x00, 0x01,
	    0x3D, 0x80, 0x80, 0x03, 0x61, 0x8C, 0x41, 0x8C, 0x7D, 0x89, 0x03, 0xA6, 0x4E, 0x80, 0x04, 0x21, 0x2C, 0x03,
	    0x00, 0x00, 0x41, 0x82, 0x00, 0x78, 0x80, 0x83, 0x00, 0x2C, 0x80, 0x84, 0x1E, 0xCC, 0xC0, 0x3F, 0x00, 0x2C,
	    0xD0, 0x24, 0x00, 0x18, 0xC0, 0x5E, 0x00, 0x04, 0xFC, 0x01, 0x10, 0x40, 0x41, 0x81, 0x00, 0x0C, 0x38, 0x60,
	    0x00, 0x80, 0x48, 0x00, 0x00, 0x08, 0x38, 0x60, 0x00, 0x7F, 0x98, 0x64, 0x00, 0x06, 0x48, 0x00, 0x00, 0x48,
	    0x7C, 0x85, 0x23, 0x78, 0x38, 0x63, 0xFF, 0xFF, 0x2C, 0x03, 0x00, 0x00, 0x40, 0x80, 0x00, 0x08, 0x38, 0x63,
	    0x00, 0x05, 0x3C, 0x80, 0x80, 0x46, 0x60, 0x84, 0xB1, 0x08, 0x1C, 0x63, 0x00, 0x30, 0x7C, 0x84, 0x1A, 0x14,
	    0x1C, 0x65, 0x00, 0x0C, 0x7C, 0x84, 0x1A, 0x14, 0x88, 0x64, 0x00, 0x02, 0x7C, 0x63, 0x07, 0x74, 0x4E, 0x80,
	    0x00, 0x20, 0x4E, 0x80, 0x00, 0x21, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xBA, 0x81, 0x00, 0x08,
	    0x80, 0x01, 0x00, 0xB4, 0x38, 0x21, 0x00, 0xB0, 0x7C, 0x08, 0x03, 0xA6, 0x00, 0x00, 0x00, 0x00, 0xC2, 0x09,
	    0x98, 0xA4, 0x00, 0x00, 0x00, 0x2B, // #External/UCF + Arduino Toggle UI/UCF/UCF
	                                        // 0.74 Shield Drop - Check for Toggle.asm
	    0x7C, 0x08, 0x02, 0xA6, 0x90, 0x01, 0x00, 0x04, 0x94, 0x21, 0xFF, 0x50, 0xBE, 0x81, 0x00, 0x08, 0x7C, 0x7E,
	    0x1B, 0x78, 0x83, 0xFE, 0x00, 0x2C, 0x48, 0x00, 0x01, 0x01, 0x7F, 0xA8, 0x02, 0xA6, 0x88, 0x9F, 0x06, 0x18,
	    0x38, 0x62, 0xF2, 0x28, 0x7C, 0x63, 0x20, 0xAE, 0x2C, 0x03, 0x00, 0x01, 0x41, 0x82, 0x00, 0x14, 0x38, 0x62,
	    0xF2, 0x30, 0x7C, 0x63, 0x20, 0xAE, 0x2C, 0x03, 0x00, 0x01, 0x40, 0x82, 0x00, 0xF8, 0xC0, 0x3F, 0x06, 0x3C,
	    0x80, 0x6D, 0xAE, 0xB4, 0xC0, 0x03, 0x03, 0x14, 0xFC, 0x01, 0x00, 0x40, 0x40, 0x81, 0x00, 0xE4, 0xC0, 0x3F,
	    0x06, 0x20, 0x48, 0x00, 0x00, 0x71, 0xD0, 0x21, 0x00, 0x90, 0xC0, 0x3F, 0x06, 0x24, 0x48, 0x00, 0x00, 0x65,
	    0xC0, 0x41, 0x00, 0x90, 0xEC, 0x42, 0x00, 0xB2, 0xEC, 0x21, 0x00, 0x72, 0xEC, 0x21, 0x10, 0x2A, 0xC0, 0x5D,
	    0x00, 0x0C, 0xFC, 0x01, 0x10, 0x40, 0x41, 0x80, 0x00, 0xB4, 0x88, 0x9F, 0x06, 0x70, 0x2C, 0x04, 0x00, 0x03,
	    0x40, 0x81, 0x00, 0xA8, 0xC0, 0x1D, 0x00, 0x10, 0xC0, 0x3F, 0x06, 0x24, 0xFC, 0x00, 0x08, 0x40, 0x40, 0x80,
	    0x00, 0x98, 0xBA, 0x81, 0x00, 0x08, 0x80, 0x01, 0x00, 0xB4, 0x38, 0x21, 0x00, 0xB0, 0x7C, 0x08, 0x03, 0xA6,
	    0x80, 0x61, 0x00, 0x1C, 0x83, 0xE1, 0x00, 0x14, 0x38, 0x21, 0x00, 0x18, 0x38, 0x63, 0x00, 0x08, 0x7C, 0x68,
	    0x03, 0xA6, 0x4E, 0x80, 0x00, 0x20, 0xFC, 0x00, 0x0A, 0x10, 0xC0, 0x3D, 0x00, 0x00, 0xEC, 0x00, 0x00, 0x72,
	    0xC0, 0x3D, 0x00, 0x04, 0xEC, 0x00, 0x08, 0x28, 0xFC, 0x00, 0x00, 0x1E, 0xD8, 0x01, 0x00, 0x80, 0x80, 0x61,
	    0x00, 0x84, 0x38, 0x63, 0x00, 0x02, 0x3C, 0x00, 0x43, 0x30, 0xC8, 0x5D, 0x00, 0x14, 0x6C, 0x63, 0x80, 0x00,
	    0x90, 0x01, 0x00, 0x80, 0x90, 0x61, 0x00, 0x84, 0xC8, 0x21, 0x00, 0x80, 0xEC, 0x01, 0x10, 0x28, 0xC0, 0x3D,
	    0x00, 0x00, 0xEC, 0x20, 0x08, 0x24, 0x4E, 0x80, 0x00, 0x20, 0x4E, 0x80, 0x00, 0x21, 0x42, 0xA0, 0x00, 0x00,
	    0x37, 0x27, 0x00, 0x00, 0x43, 0x30, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0xBF, 0x4C, 0xCC, 0xCD, 0x43, 0x30,
	    0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x7F, 0xC3, 0xF3, 0x78, 0x7F, 0xE4, 0xFB, 0x78, 0xBA, 0x81, 0x00, 0x08,
	    0x80, 0x01, 0x00, 0xB4, 0x38, 0x21, 0x00, 0xB0, 0x7C, 0x08, 0x03, 0xA6, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00,
	    0x00, 0x00, 0xC2, 0x16, 0xE7, 0x50, 0x00, 0x00, 0x00,
	    0x33, // #Common/StaticPatches/ToggledStaticOverwrites.asm
	    0x88, 0x62, 0xF2, 0x34, 0x2C, 0x03, 0x00, 0x00, 0x41, 0x82, 0x00, 0x14, 0x48, 0x00, 0x00, 0x75, 0x7C, 0x68,
	    0x02, 0xA6, 0x48, 0x00, 0x01, 0x3D, 0x48, 0x00, 0x00, 0x14, 0x48, 0x00, 0x00, 0x95, 0x7C, 0x68, 0x02, 0xA6,
	    0x48, 0x00, 0x01, 0x2D, 0x48, 0x00, 0x00, 0x04, 0x88, 0x62, 0xF2, 0x38, 0x2C, 0x03, 0x00, 0x00, 0x41, 0x82,
	    0x00, 0x14, 0x48, 0x00, 0x00, 0xB9, 0x7C, 0x68, 0x02, 0xA6, 0x48, 0x00, 0x01, 0x11, 0x48, 0x00, 0x00, 0x10,
	    0x48, 0x00, 0x00, 0xC9, 0x7C, 0x68, 0x02, 0xA6, 0x48, 0x00, 0x01, 0x01, 0x88, 0x62, 0xF2, 0x3C, 0x2C, 0x03,
	    0x00, 0x00, 0x41, 0x82, 0x00, 0x14, 0x48, 0x00, 0x00, 0xD1, 0x7C, 0x68, 0x02, 0xA6, 0x48, 0x00, 0x00, 0xE9,
	    0x48, 0x00, 0x01, 0x04, 0x48, 0x00, 0x00, 0xD1, 0x7C, 0x68, 0x02, 0xA6, 0x48, 0x00, 0x00, 0xD9, 0x48, 0x00,
	    0x00, 0xF4, 0x4E, 0x80, 0x00, 0x21, 0x80, 0x3C, 0xE4, 0xD4, 0x00, 0x24, 0x04, 0x64, 0x80, 0x07, 0x96, 0xE0,
	    0x60, 0x00, 0x00, 0x00, 0x80, 0x2B, 0x7E, 0x54, 0x48, 0x00, 0x00, 0x88, 0x80, 0x2B, 0x80, 0x8C, 0x48, 0x00,
	    0x00, 0x84, 0x80, 0x12, 0x39, 0xA8, 0x60, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x4E, 0x80, 0x00, 0x21,
	    0x80, 0x3C, 0xE4, 0xD4, 0x00, 0x20, 0x00, 0x00, 0x80, 0x07, 0x96, 0xE0, 0x3A, 0x40, 0x00, 0x01, 0x80, 0x2B,
	    0x7E, 0x54, 0x88, 0x7F, 0x22, 0x40, 0x80, 0x2B, 0x80, 0x8C, 0x2C, 0x03, 0x00, 0x02, 0x80, 0x10, 0xFC, 0x48,
	    0x90, 0x05, 0x21, 0xDC, 0x80, 0x10, 0xFB, 0x68, 0x90, 0x05, 0x21, 0xDC, 0x80, 0x12, 0x39, 0xA8, 0x90, 0x1F,
	    0x1A, 0x5C, 0xFF, 0xFF, 0xFF, 0xFF, 0x4E, 0x80, 0x00, 0x21, 0x80, 0x1D, 0x46, 0x10, 0x48, 0x00, 0x00, 0x4C,
	    0x80, 0x1D, 0x47, 0x24, 0x48, 0x00, 0x00, 0x3C, 0x80, 0x1D, 0x46, 0x0C, 0x80, 0x9F, 0x00, 0xEC, 0xFF, 0xFF,
	    0xFF, 0xFF, 0x4E, 0x80, 0x00, 0x21, 0x80, 0x1D, 0x46, 0x10, 0x38, 0x83, 0x7F, 0x9C, 0x80, 0x1D, 0x47, 0x24,
	    0x88, 0x1B, 0x00, 0xC4, 0x80, 0x1D, 0x46, 0x0C, 0x3C, 0x60, 0x80, 0x3B, 0xFF, 0xFF, 0xFF, 0xFF, 0x4E, 0x80,
	    0x00, 0x21, 0x80, 0x1D, 0x45, 0xFC, 0x48, 0x00, 0x09, 0xDC, 0xFF, 0xFF, 0xFF, 0xFF, 0x4E, 0x80, 0x00, 0x21,
	    0x80, 0x1D, 0x45, 0xFC, 0x40, 0x80, 0x09, 0xDC, 0xFF, 0xFF, 0xFF, 0xFF, 0x38, 0xA3, 0xFF, 0xFC, 0x84, 0x65,
	    0x00, 0x04, 0x2C, 0x03, 0xFF, 0xFF, 0x41, 0x82, 0x00, 0x10, 0x84, 0x85, 0x00, 0x04, 0x90, 0x83, 0x00, 0x00,
	    0x4B, 0xFF, 0xFF, 0xEC, 0x4E, 0x80, 0x00, 0x20, 0x3C, 0x60, 0x80, 0x00, 0x3C, 0x80, 0x00, 0x3B, 0x60, 0x84,
	    0x72, 0x2C, 0x3D, 0x80, 0x80, 0x32, 0x61, 0x8C, 0x8F, 0x50, 0x7D, 0x89, 0x03, 0xA6, 0x4E, 0x80, 0x04, 0x21,
	    0x3C, 0x60, 0x80, 0x17, 0x3C, 0x80, 0x80, 0x17, 0x00, 0x00, 0x00, 0x00, 0xC2, 0x1D, 0x14, 0xC8, 0x00, 0x00,
	    0x00, 0x04, // #Common/Preload Stadium
	                // Transformations/Handlers/Init
	                // isLoaded Bool.asm
	    0x88, 0x62, 0xF2, 0x38, 0x2C, 0x03, 0x00, 0x00, 0x41, 0x82, 0x00, 0x0C, 0x38, 0x60, 0x00, 0x00, 0x98, 0x7F,
	    0x00, 0xF0, 0x3B, 0xA0, 0x00, 0x01, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC2, 0x1D, 0x45, 0xEC,
	    0x00, 0x00, 0x00, 0x1B, // #Common/Preload Stadium
	                            // Transformations/Handlers/Load
	                            // Transformation.asm
	    0x88, 0x62, 0xF2, 0x38, 0x2C, 0x03, 0x00, 0x00, 0x41, 0x82, 0x00, 0xC4, 0x88, 0x7F, 0x00, 0xF0, 0x2C, 0x03,
	    0x00, 0x00, 0x40, 0x82, 0x00, 0xB8, 0x38, 0x60, 0x00, 0x04, 0x3D, 0x80, 0x80, 0x38, 0x61, 0x8C, 0x05, 0x80,
	    0x7D, 0x89, 0x03, 0xA6, 0x4E, 0x80, 0x04, 0x21, 0x54, 0x60, 0x10, 0x3A, 0xA8, 0x7F, 0x00, 0xE2, 0x3C, 0x80,
	    0x80, 0x3B, 0x60, 0x84, 0x7F, 0x9C, 0x7C, 0x84, 0x00, 0x2E, 0x7C, 0x03, 0x20, 0x00, 0x41, 0x82, 0xFF, 0xD4,
	    0x90, 0x9F, 0x00, 0xEC, 0x2C, 0x04, 0x00, 0x03, 0x40, 0x82, 0x00, 0x0C, 0x38, 0x80, 0x00, 0x00, 0x48, 0x00,
	    0x00, 0x34, 0x2C, 0x04, 0x00, 0x04, 0x40, 0x82, 0x00, 0x0C, 0x38, 0x80, 0x00, 0x01, 0x48, 0x00, 0x00, 0x24,
	    0x2C, 0x04, 0x00, 0x09, 0x40, 0x82, 0x00, 0x0C, 0x38, 0x80, 0x00, 0x02, 0x48, 0x00, 0x00, 0x14, 0x2C, 0x04,
	    0x00, 0x06, 0x40, 0x82, 0x00, 0x00, 0x38, 0x80, 0x00, 0x03, 0x48, 0x00, 0x00, 0x04, 0x3C, 0x60, 0x80, 0x3E,
	    0x60, 0x63, 0x12, 0x48, 0x54, 0x80, 0x10, 0x3A, 0x7C, 0x63, 0x02, 0x14, 0x80, 0x63, 0x03, 0xD8, 0x80, 0x9F,
	    0x00, 0xCC, 0x38, 0xBF, 0x00, 0xC8, 0x3C, 0xC0, 0x80, 0x1D, 0x60, 0xC6, 0x42, 0x20, 0x38, 0xE0, 0x00, 0x00,
	    0x3D, 0x80, 0x80, 0x01, 0x61, 0x8C, 0x65, 0x80, 0x7D, 0x89, 0x03, 0xA6, 0x4E, 0x80, 0x04, 0x21, 0x38, 0x60,
	    0x00, 0x01, 0x98, 0x7F, 0x00, 0xF0, 0x80, 0x7F, 0x00, 0xD8, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	    0xC2, 0x1D, 0x4F, 0x14, 0x00, 0x00, 0x00, 0x04, // #Common/Preload
	                                                    // Stadium
	                                                    // Transformations/Handlers/Reset
	                                                    // isLoaded.asm
	    0x88, 0x62, 0xF2, 0x38, 0x2C, 0x03, 0x00, 0x00, 0x41, 0x82, 0x00, 0x0C, 0x38, 0x60, 0x00, 0x00, 0x98, 0x7F,
	    0x00, 0xF0, 0x80, 0x6D, 0xB2, 0xD8, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC2, 0x06, 0x8F, 0x30,
	    0x00, 0x00, 0x00, 0x9D, // #Common/PAL/Handlers/Character DAT
	                            // Patcher.asm
	    0x88, 0x62, 0xF2, 0x34, 0x2C, 0x03, 0x00, 0x00, 0x41, 0x82, 0x04, 0xD4, 0x7C, 0x08, 0x02, 0xA6, 0x90, 0x01,
	    0x00, 0x04, 0x94, 0x21, 0xFF, 0x50, 0xBE, 0x81, 0x00, 0x08, 0x83, 0xFE, 0x01, 0x0C, 0x83, 0xFF, 0x00, 0x08,
	    0x3B, 0xFF, 0xFF, 0xE0, 0x80, 0x7D, 0x00, 0x00, 0x2C, 0x03, 0x00, 0x1B, 0x40, 0x80, 0x04, 0x9C, 0x48, 0x00,
	    0x00, 0x71, 0x48, 0x00, 0x00, 0xA9, 0x48, 0x00, 0x00, 0xB9, 0x48, 0x00, 0x01, 0x51, 0x48, 0x00, 0x01, 0x79,
	    0x48, 0x00, 0x01, 0x79, 0x48, 0x00, 0x02, 0x29, 0x48, 0x00, 0x02, 0x39, 0x48, 0x00, 0x02, 0x81, 0x48, 0x00,
	    0x02, 0xF9, 0x48, 0x00, 0x03, 0x11, 0x48, 0x00, 0x03, 0x11, 0x48, 0x00, 0x03, 0x11, 0x48, 0x00, 0x03, 0x11,
	    0x48, 0x00, 0x03, 0x21, 0x48, 0x00, 0x03, 0x21, 0x48, 0x00, 0x03, 0x89, 0x48, 0x00, 0x03, 0x89, 0x48, 0x00,
	    0x03, 0x91, 0x48, 0x00, 0x03, 0x91, 0x48, 0x00, 0x03, 0xA9, 0x48, 0x00, 0x03, 0xA9, 0x48, 0x00, 0x03, 0xB9,
	    0x48, 0x00, 0x03, 0xB9, 0x48, 0x00, 0x03, 0xC9, 0x48, 0x00, 0x03, 0xC9, 0x48, 0x00, 0x03, 0xC9, 0x48, 0x00,
	    0x04, 0x29, 0x7C, 0x88, 0x02, 0xA6, 0x1C, 0x63, 0x00, 0x04, 0x7C, 0x84, 0x1A, 0x14, 0x80, 0xA4, 0x00, 0x00,
	    0x54, 0xA5, 0x01, 0xBA, 0x7C, 0xA4, 0x2A, 0x14, 0x80, 0x65, 0x00, 0x00, 0x80, 0x85, 0x00, 0x04, 0x2C, 0x03,
	    0x00, 0xFF, 0x41, 0x82, 0x00, 0x14, 0x7C, 0x63, 0xFA, 0x14, 0x90, 0x83, 0x00, 0x00, 0x38, 0xA5, 0x00, 0x08,
	    0x4B, 0xFF, 0xFF, 0xE4, 0x48, 0x00, 0x03, 0xF0, 0x00, 0x00, 0x33, 0x44, 0x3F, 0x54, 0x7A, 0xE1, 0x00, 0x00,
	    0x33, 0x60, 0x42, 0xC4, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x37, 0x9C, 0x42, 0x92, 0x00, 0x00,
	    0x00, 0x00, 0x39, 0x08, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x39, 0x0C, 0x40, 0x86, 0x66, 0x66, 0x00, 0x00,
	    0x39, 0x10, 0x3D, 0xEA, 0x0E, 0xA1, 0x00, 0x00, 0x39, 0x28, 0x41, 0xA0, 0x00, 0x00, 0x00, 0x00, 0x3C, 0x04,
	    0x2C, 0x01, 0x48, 0x0C, 0x00, 0x00, 0x47, 0x20, 0x1B, 0x96, 0x80, 0x13, 0x00, 0x00, 0x47, 0x34, 0x1B, 0x96,
	    0x80, 0x13, 0x00, 0x00, 0x47, 0x3C, 0x04, 0x00, 0x00, 0x09, 0x00, 0x00, 0x4A, 0x40, 0x2C, 0x00, 0x68, 0x11,
	    0x00, 0x00, 0x4A, 0x4C, 0x28, 0x1B, 0x00, 0x13, 0x00, 0x00, 0x4A, 0x50, 0x0D, 0x00, 0x01, 0x0B, 0x00, 0x00,
	    0x4A, 0x54, 0x2C, 0x80, 0x68, 0x11, 0x00, 0x00, 0x4A, 0x60, 0x28, 0x1B, 0x00, 0x13, 0x00, 0x00, 0x4A, 0x64,
	    0x0D, 0x00, 0x01, 0x0B, 0x00, 0x00, 0x4B, 0x24, 0x2C, 0x00, 0x68, 0x0D, 0x00, 0x00, 0x4B, 0x30, 0x0F, 0x10,
	    0x40, 0x13, 0x00, 0x00, 0x4B, 0x38, 0x2C, 0x80, 0x38, 0x0D, 0x00, 0x00, 0x4B, 0x44, 0x0F, 0x10, 0x40, 0x13,
	    0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x38, 0x0C, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x4E, 0xF8, 0x2C, 0x00,
	    0x38, 0x03, 0x00, 0x00, 0x4F, 0x08, 0x0F, 0x80, 0x00, 0x0B, 0x00, 0x00, 0x4F, 0x0C, 0x2C, 0x80, 0x20, 0x03,
	    0x00, 0x00, 0x4F, 0x1C, 0x0F, 0x80, 0x00, 0x0B, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00,
	    0x4D, 0x10, 0x3F, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x4D, 0x70, 0x42, 0x94, 0x00, 0x00, 0x00, 0x00, 0x4D, 0xD4,
	    0x41, 0x90, 0x00, 0x00, 0x00, 0x00, 0x4D, 0xE0, 0x41, 0x90, 0x00, 0x00, 0x00, 0x00, 0x83, 0xAC, 0x2C, 0x00,
	    0x00, 0x09, 0x00, 0x00, 0x83, 0xB8, 0x34, 0x8C, 0x80, 0x11, 0x00, 0x00, 0x84, 0x00, 0x34, 0x8C, 0x80, 0x11,
	    0x00, 0x00, 0x84, 0x30, 0x05, 0x00, 0x00, 0x8B, 0x00, 0x00, 0x84, 0x38, 0x04, 0x1A, 0x05, 0x00, 0x00, 0x00,
	    0x84, 0x44, 0x05, 0x00, 0x00, 0x8B, 0x00, 0x00, 0x84, 0xDC, 0x05, 0x78, 0x05, 0x78, 0x00, 0x00, 0x85, 0xB8,
	    0x10, 0x00, 0x01, 0x0B, 0x00, 0x00, 0x85, 0xC0, 0x03, 0xE8, 0x01, 0xF4, 0x00, 0x00, 0x85, 0xCC, 0x10, 0x00,
	    0x01, 0x0B, 0x00, 0x00, 0x85, 0xD4, 0x03, 0x84, 0x03, 0xE8, 0x00, 0x00, 0x85, 0xE0, 0x10, 0x00, 0x01, 0x0B,
	    0x00, 0x00, 0x88, 0x18, 0x0B, 0x00, 0x01, 0x0B, 0x00, 0x00, 0x88, 0x2C, 0x0B, 0x00, 0x01, 0x0B, 0x00, 0x00,
	    0x88, 0xF8, 0x04, 0x1A, 0x0B, 0xB8, 0x00, 0x00, 0x89, 0x3C, 0x04, 0x1A, 0x0B, 0xB8, 0x00, 0x00, 0x89, 0x80,
	    0x04, 0x1A, 0x0B, 0xB8, 0x00, 0x00, 0x89, 0xE0, 0x04, 0xFE, 0xF7, 0x04, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00,
	    0x36, 0xCC, 0x42, 0xEC, 0x00, 0x00, 0x00, 0x00, 0x37, 0xC4, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF,
	    0x00, 0x00, 0x34, 0x68, 0x3F, 0x66, 0x66, 0x66, 0x00, 0x00, 0x39, 0xD8, 0x44, 0x0C, 0x00, 0x00, 0x00, 0x00,
	    0x3A, 0x44, 0xB4, 0x99, 0x00, 0x11, 0x00, 0x00, 0x3A, 0x48, 0x1B, 0x8C, 0x00, 0x8F, 0x00, 0x00, 0x3A, 0x58,
	    0xB4, 0x99, 0x00, 0x11, 0x00, 0x00, 0x3A, 0x5C, 0x1B, 0x8C, 0x00, 0x8F, 0x00, 0x00, 0x3A, 0x6C, 0xB4, 0x99,
	    0x00, 0x11, 0x00, 0x00, 0x3A, 0x70, 0x1B, 0x8C, 0x00, 0x8F, 0x00, 0x00, 0x3B, 0x30, 0x44, 0x0C, 0x00, 0x00,
	    0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x45, 0xC8, 0x2C, 0x01, 0x50, 0x10, 0x00, 0x00, 0x45, 0xD4, 0x2D, 0x19,
	    0x80, 0x13, 0x00, 0x00, 0x45, 0xDC, 0x2C, 0x80, 0xB0, 0x10, 0x00, 0x00, 0x45, 0xE8, 0x2D, 0x19, 0x80, 0x13,
	    0x00, 0x00, 0x49, 0xC4, 0x2C, 0x00, 0x68, 0x0A, 0x00, 0x00, 0x49, 0xD0, 0x28, 0x1B, 0x80, 0x13, 0x00, 0x00,
	    0x49, 0xD8, 0x2C, 0x80, 0x78, 0x0A, 0x00, 0x00, 0x49, 0xE4, 0x28, 0x1B, 0x80, 0x13, 0x00, 0x00, 0x49, 0xF0,
	    0x2C, 0x00, 0x68, 0x08, 0x00, 0x00, 0x49, 0xFC, 0x23, 0x1B, 0x80, 0x13, 0x00, 0x00, 0x4A, 0x04, 0x2C, 0x80,
	    0x78, 0x08, 0x00, 0x00, 0x4A, 0x10, 0x23, 0x1B, 0x80, 0x13, 0x00, 0x00, 0x5C, 0x98, 0x1E, 0x0C, 0x80, 0x80,
	    0x00, 0x00, 0x5C, 0xF4, 0xB4, 0x80, 0x0C, 0x90, 0x00, 0x00, 0x5D, 0x08, 0xB4, 0x80, 0x0C, 0x90, 0x00, 0x00,
	    0x00, 0xFF, 0x00, 0x00, 0x3A, 0x1C, 0xB4, 0x94, 0x00, 0x13, 0x00, 0x00, 0x3A, 0x64, 0x2C, 0x00, 0x00, 0x15,
	    0x00, 0x00, 0x3A, 0x70, 0xB4, 0x92, 0x80, 0x13, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00,
	    0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x64, 0x7C, 0xB4, 0x9A, 0x40, 0x17, 0x00, 0x00, 0x64, 0x80,
	    0x64, 0x00, 0x10, 0x97, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x33, 0xE4, 0x42, 0xDE,
	    0x00, 0x00, 0x00, 0x00, 0x45, 0x28, 0x2C, 0x01, 0x30, 0x11, 0x00, 0x00, 0x45, 0x34, 0xB4, 0x98, 0x80, 0x13,
	    0x00, 0x00, 0x45, 0x3C, 0x2C, 0x81, 0x30, 0x11, 0x00, 0x00, 0x45, 0x48, 0xB4, 0x98, 0x80, 0x13, 0x00, 0x00,
	    0x45, 0x50, 0x2D, 0x00, 0x20, 0x11, 0x00, 0x00, 0x45, 0x5C, 0xB4, 0x98, 0x80, 0x13, 0x00, 0x00, 0x45, 0xF8,
	    0x2C, 0x01, 0x30, 0x0F, 0x00, 0x00, 0x46, 0x08, 0x0F, 0x00, 0x01, 0x0B, 0x00, 0x00, 0x46, 0x0C, 0x2C, 0x81,
	    0x28, 0x0F, 0x00, 0x00, 0x46, 0x1C, 0x0F, 0x00, 0x01, 0x0B, 0x00, 0x00, 0x4A, 0xEC, 0x2C, 0x00, 0x70, 0x03,
	    0x00, 0x00, 0x4B, 0x00, 0x2C, 0x80, 0x38, 0x03, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00,
	    0x48, 0x5C, 0x2C, 0x00, 0x00, 0x0F, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x37, 0xB0,
	    0x3F, 0x59, 0x99, 0x9A, 0x00, 0x00, 0x37, 0xCC, 0x42, 0xAA, 0x00, 0x00, 0x00, 0x00, 0x55, 0x20, 0x87, 0x11,
	    0x80, 0x13, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x3B, 0x8C, 0x44, 0x0C, 0x00, 0x00,
	    0x00, 0x00, 0x3D, 0x0C, 0x44, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00,
	    0x50, 0xE4, 0xB4, 0x99, 0x00, 0x13, 0x00, 0x00, 0x50, 0xF8, 0xB4, 0x99, 0x00, 0x13, 0x00, 0x00, 0x00, 0xFF,
	    0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x4E, 0xB0, 0x02, 0xBC, 0xFF, 0x38, 0x00, 0x00,
	    0x4E, 0xBC, 0x14, 0x00, 0x01, 0x23, 0x00, 0x00, 0x4E, 0xC4, 0x03, 0x84, 0x01, 0xF4, 0x00, 0x00, 0x4E, 0xD0,
	    0x14, 0x00, 0x01, 0x23, 0x00, 0x00, 0x4E, 0xD8, 0x04, 0x4C, 0x04, 0xB0, 0x00, 0x00, 0x4E, 0xE4, 0x14, 0x00,
	    0x01, 0x23, 0x00, 0x00, 0x50, 0x5C, 0x2C, 0x00, 0x68, 0x15, 0x00, 0x00, 0x50, 0x6C, 0x14, 0x08, 0x01, 0x23,
	    0x00, 0x00, 0x50, 0x70, 0x2C, 0x80, 0x60, 0x15, 0x00, 0x00, 0x50, 0x80, 0x14, 0x08, 0x01, 0x23, 0x00, 0x00,
	    0x50, 0x84, 0x2D, 0x00, 0x20, 0x15, 0x00, 0x00, 0x50, 0x94, 0x14, 0x08, 0x01, 0x23, 0x00, 0x00, 0x00, 0xFF,
	    0x00, 0x00, 0x00, 0xFF, 0xBA, 0x81, 0x00, 0x08, 0x80, 0x01, 0x00, 0xB4, 0x38, 0x21, 0x00, 0xB0, 0x7C, 0x08,
	    0x03, 0xA6, 0x3C, 0x60, 0x80, 0x3C, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC2, 0x2F, 0x9A, 0x3C,
	    0x00, 0x00, 0x00, 0x08, // #Common/PAL/Handlers/PAL Stock Icons.asm
	    0x88, 0x62, 0xF2, 0x34, 0x2C, 0x03, 0x00, 0x00, 0x41, 0x82, 0x00, 0x30, 0x48, 0x00, 0x00, 0x21, 0x7C, 0x88,
	    0x02, 0xA6, 0x80, 0x64, 0x00, 0x00, 0x90, 0x7D, 0x00, 0x2C, 0x90, 0x7D, 0x00, 0x30, 0x80, 0x64, 0x00, 0x04,
	    0x90, 0x7D, 0x00, 0x3C, 0x48, 0x00, 0x00, 0x10, 0x4E, 0x80, 0x00, 0x21, 0x3F, 0x59, 0x99, 0x9A, 0xC1, 0xA8,
	    0x00, 0x00, 0x80, 0x1D, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0xC2, 0x10, 0xFC, 0x44, 0x00, 0x00, 0x00,
	    0x04, // #Common/PAL/Handlers/DK
	          // Up B/Aerial Up B.asm
	    0x88, 0x82, 0xF2, 0x34, 0x2C, 0x04, 0x00, 0x00, 0x41, 0x82, 0x00, 0x10, 0x3C, 0x00, 0x80, 0x11, 0x60, 0x00,
	    0x00, 0x74, 0x48, 0x00, 0x00, 0x08, 0x38, 0x03, 0xD7, 0x74, 0x00, 0x00, 0x00, 0x00, 0xC2, 0x10, 0xFB, 0x64,
	    0x00, 0x00, 0x00, 0x04, // #Common/PAL/Handlers/DK Up B/Grounded
	                            // Up B.asm
	    0x88, 0x82, 0xF2, 0x34, 0x2C, 0x04, 0x00, 0x00, 0x41, 0x82, 0x00, 0x10, 0x3C, 0x00, 0x80, 0x11, 0x60, 0x00,
	    0x00, 0x74, 0x48, 0x00, 0x00, 0x08, 0x38, 0x03, 0xD7, 0x74, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00,
	    0x00, 0x00, 0x00, 0x00 // Termination sequence
	};
}

SlippiPlaybackStatus::~SlippiPlaybackStatus()
	{
	// Kill threads to prevent cleanup crash
	resetPlayback();
}