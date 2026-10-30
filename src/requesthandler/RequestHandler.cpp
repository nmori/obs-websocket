/*
obs-websocket
Copyright (C) 2016-2021 Stephane Lepin <stephane.lepin@gmail.com>
Copyright (C) 2020-2021 Kyle Manning <tt2468@gmail.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#ifdef PLUGIN_TESTS
#include <util/profiler.hpp>
#endif

#include <chrono>
#include <thread>
#include <QApplication>
#include <QWidget>

#include "RequestHandler.h"
#include "../eventhandler/EventHandler.h"

// Requests which do not touch frontend state and may block for a long time, so they are not moved to the UI thread
const std::unordered_set<std::string> RequestHandler::_callingThreadRequests{
	"GetVersion", "Sleep", "CallVendorRequest", "GetSourceScreenshot", "SaveSourceScreenshot",
};

// Requests which make OBS rebuild or close properties and filters windows (filter list changes, source removal...)
const std::unordered_set<std::string> RequestHandler::_propertiesRebuildingRequests{
	"CreateSourceFilter", "RemoveSourceFilter",    "SetSourceFilterIndex",      "RemoveInput",
	"RemoveScene",        "CreateSceneCollection", "SetCurrentSceneCollection",
};

// True while a modal dialog (font, color, file picker, message box...) opened from a window containing a properties view
// is running its nested event loop. Rebuilding or closing that window meanwhile destroys the widgets the dialog returns
// to, which crashes OBS once the dialog is closed.
static bool IsPropertiesDialogOpen()
{
	QWidget *modal = QApplication::activeModalWidget();
	if (!modal || !modal->parentWidget())
		return false;

	QWidget *window = modal->parentWidget()->window();
	for (QWidget *child : window->findChildren<QWidget *>())
		if (child->inherits("OBSPropertiesView") && child->window() == window)
			return true;

	return false;
}

const std::unordered_map<std::string, RequestMethodHandler> RequestHandler::_handlerMap{
	// General
	{"GetVersion", &RequestHandler::GetVersion},
	{"GetStats", &RequestHandler::GetStats},
	{"BroadcastCustomEvent", &RequestHandler::BroadcastCustomEvent},
	{"CallVendorRequest", &RequestHandler::CallVendorRequest},
	{"GetHotkeyList", &RequestHandler::GetHotkeyList},
	{"TriggerHotkeyByName", &RequestHandler::TriggerHotkeyByName},
	{"TriggerHotkeyByKeySequence", &RequestHandler::TriggerHotkeyByKeySequence},
	{"Sleep", &RequestHandler::Sleep},

	// Config
	{"GetPersistentData", &RequestHandler::GetPersistentData},
	{"SetPersistentData", &RequestHandler::SetPersistentData},
	{"GetSceneCollectionList", &RequestHandler::GetSceneCollectionList},
	{"SetCurrentSceneCollection", &RequestHandler::SetCurrentSceneCollection},
	{"CreateSceneCollection", &RequestHandler::CreateSceneCollection},
	{"GetProfileList", &RequestHandler::GetProfileList},
	{"SetCurrentProfile", &RequestHandler::SetCurrentProfile},
	{"CreateProfile", &RequestHandler::CreateProfile},
	{"RemoveProfile", &RequestHandler::RemoveProfile},
	{"GetProfileParameter", &RequestHandler::GetProfileParameter},
	{"SetProfileParameter", &RequestHandler::SetProfileParameter},
	{"GetVideoSettings", &RequestHandler::GetVideoSettings},
	{"SetVideoSettings", &RequestHandler::SetVideoSettings},
	{"GetStreamServiceSettings", &RequestHandler::GetStreamServiceSettings},
	{"SetStreamServiceSettings", &RequestHandler::SetStreamServiceSettings},
	{"GetRecordDirectory", &RequestHandler::GetRecordDirectory},
	{"SetRecordDirectory", &RequestHandler::SetRecordDirectory},

	// Canvases
	{"GetCanvasList", &RequestHandler::GetCanvasList},

	// Sources
	{"GetSourceActive", &RequestHandler::GetSourceActive},
	{"GetSourceScreenshot", &RequestHandler::GetSourceScreenshot},
	{"SaveSourceScreenshot", &RequestHandler::SaveSourceScreenshot},
	{"GetSourcePrivateSettings", &RequestHandler::GetSourcePrivateSettings},
	{"SetSourcePrivateSettings", &RequestHandler::SetSourcePrivateSettings},

	// Scenes
	{"GetSceneList", &RequestHandler::GetSceneList},
	{"GetGroupList", &RequestHandler::GetGroupList},
	{"GetCurrentProgramScene", &RequestHandler::GetCurrentProgramScene},
	{"SetCurrentProgramScene", &RequestHandler::SetCurrentProgramScene},
	{"GetCurrentPreviewScene", &RequestHandler::GetCurrentPreviewScene},
	{"SetCurrentPreviewScene", &RequestHandler::SetCurrentPreviewScene},
	{"CreateScene", &RequestHandler::CreateScene},
	{"RemoveScene", &RequestHandler::RemoveScene},
	{"SetSceneName", &RequestHandler::SetSceneName},
	{"GetSceneSceneTransitionOverride", &RequestHandler::GetSceneSceneTransitionOverride},
	{"SetSceneSceneTransitionOverride", &RequestHandler::SetSceneSceneTransitionOverride},

	// Inputs
	{"GetInputList", &RequestHandler::GetInputList},
	{"GetInputKindList", &RequestHandler::GetInputKindList},
	{"GetSpecialInputs", &RequestHandler::GetSpecialInputs},
	{"CreateInput", &RequestHandler::CreateInput},
	{"RemoveInput", &RequestHandler::RemoveInput},
	{"SetInputName", &RequestHandler::SetInputName},
	{"GetInputDefaultSettings", &RequestHandler::GetInputDefaultSettings},
	{"GetInputSettings", &RequestHandler::GetInputSettings},
	{"SetInputSettings", &RequestHandler::SetInputSettings},
	{"GetInputMute", &RequestHandler::GetInputMute},
	{"SetInputMute", &RequestHandler::SetInputMute},
	{"ToggleInputMute", &RequestHandler::ToggleInputMute},
	{"GetInputVolume", &RequestHandler::GetInputVolume},
	{"SetInputVolume", &RequestHandler::SetInputVolume},
	{"GetInputAudioBalance", &RequestHandler::GetInputAudioBalance},
	{"SetInputAudioBalance", &RequestHandler::SetInputAudioBalance},
	{"GetInputAudioSyncOffset", &RequestHandler::GetInputAudioSyncOffset},
	{"SetInputAudioSyncOffset", &RequestHandler::SetInputAudioSyncOffset},
	{"GetInputAudioMonitorType", &RequestHandler::GetInputAudioMonitorType},
	{"SetInputAudioMonitorType", &RequestHandler::SetInputAudioMonitorType},
	{"GetInputAudioTracks", &RequestHandler::GetInputAudioTracks},
	{"SetInputAudioTracks", &RequestHandler::SetInputAudioTracks},
	{"GetInputDeinterlaceMode", &RequestHandler::GetInputDeinterlaceMode},
	{"SetInputDeinterlaceMode", &RequestHandler::SetInputDeinterlaceMode},
	{"GetInputDeinterlaceFieldOrder", &RequestHandler::GetInputDeinterlaceFieldOrder},
	{"SetInputDeinterlaceFieldOrder", &RequestHandler::SetInputDeinterlaceFieldOrder},
	{"GetInputPropertiesListPropertyItems", &RequestHandler::GetInputPropertiesListPropertyItems},
	{"PressInputPropertiesButton", &RequestHandler::PressInputPropertiesButton},

	// Transitions
	{"GetTransitionKindList", &RequestHandler::GetTransitionKindList},
	{"GetSceneTransitionList", &RequestHandler::GetSceneTransitionList},
	{"GetCurrentSceneTransition", &RequestHandler::GetCurrentSceneTransition},
	{"SetCurrentSceneTransition", &RequestHandler::SetCurrentSceneTransition},
	{"SetCurrentSceneTransitionDuration", &RequestHandler::SetCurrentSceneTransitionDuration},
	{"SetCurrentSceneTransitionSettings", &RequestHandler::SetCurrentSceneTransitionSettings},
	{"GetCurrentSceneTransitionCursor", &RequestHandler::GetCurrentSceneTransitionCursor},
	{"TriggerStudioModeTransition", &RequestHandler::TriggerStudioModeTransition},
	{"SetTBarPosition", &RequestHandler::SetTBarPosition},

	// Filters
	{"GetSourceFilterKindList", &RequestHandler::GetSourceFilterKindList},
	{"GetSourceFilterList", &RequestHandler::GetSourceFilterList},
	{"GetSourceFilterDefaultSettings", &RequestHandler::GetSourceFilterDefaultSettings},
	{"CreateSourceFilter", &RequestHandler::CreateSourceFilter},
	{"RemoveSourceFilter", &RequestHandler::RemoveSourceFilter},
	{"SetSourceFilterName", &RequestHandler::SetSourceFilterName},
	{"GetSourceFilter", &RequestHandler::GetSourceFilter},
	{"SetSourceFilterIndex", &RequestHandler::SetSourceFilterIndex},
	{"SetSourceFilterSettings", &RequestHandler::SetSourceFilterSettings},
	{"SetSourceFilterEnabled", &RequestHandler::SetSourceFilterEnabled},

	// Scene Items
	{"GetSceneItemList", &RequestHandler::GetSceneItemList},
	{"GetGroupSceneItemList", &RequestHandler::GetGroupSceneItemList},
	{"GetSceneItemId", &RequestHandler::GetSceneItemId},
	{"GetSceneItemSource", &RequestHandler::GetSceneItemSource},
	{"CreateSceneItem", &RequestHandler::CreateSceneItem},
	{"RemoveSceneItem", &RequestHandler::RemoveSceneItem},
	{"DuplicateSceneItem", &RequestHandler::DuplicateSceneItem},
	{"GetSceneItemTransform", &RequestHandler::GetSceneItemTransform},
	{"SetSceneItemTransform", &RequestHandler::SetSceneItemTransform},
	{"GetSceneItemEnabled", &RequestHandler::GetSceneItemEnabled},
	{"SetSceneItemEnabled", &RequestHandler::SetSceneItemEnabled},
	{"GetSceneItemLocked", &RequestHandler::GetSceneItemLocked},
	{"SetSceneItemLocked", &RequestHandler::SetSceneItemLocked},
	{"GetSceneItemIndex", &RequestHandler::GetSceneItemIndex},
	{"SetSceneItemIndex", &RequestHandler::SetSceneItemIndex},
	{"GetSceneItemBlendMode", &RequestHandler::GetSceneItemBlendMode},
	{"SetSceneItemBlendMode", &RequestHandler::SetSceneItemBlendMode},
	{"GetSceneItemPrivateSettings", &RequestHandler::GetSceneItemPrivateSettings},
	{"SetSceneItemPrivateSettings", &RequestHandler::SetSceneItemPrivateSettings},

	// Outputs
	{"GetVirtualCamStatus", &RequestHandler::GetVirtualCamStatus},
	{"ToggleVirtualCam", &RequestHandler::ToggleVirtualCam},
	{"StartVirtualCam", &RequestHandler::StartVirtualCam},
	{"StopVirtualCam", &RequestHandler::StopVirtualCam},
	{"GetReplayBufferStatus", &RequestHandler::GetReplayBufferStatus},
	{"ToggleReplayBuffer", &RequestHandler::ToggleReplayBuffer},
	{"StartReplayBuffer", &RequestHandler::StartReplayBuffer},
	{"StopReplayBuffer", &RequestHandler::StopReplayBuffer},
	{"SaveReplayBuffer", &RequestHandler::SaveReplayBuffer},
	{"GetLastReplayBufferReplay", &RequestHandler::GetLastReplayBufferReplay},
	{"GetOutputList", &RequestHandler::GetOutputList},
	{"GetOutputStatus", &RequestHandler::GetOutputStatus},
	{"ToggleOutput", &RequestHandler::ToggleOutput},
	{"StartOutput", &RequestHandler::StartOutput},
	{"StopOutput", &RequestHandler::StopOutput},
	{"GetOutputSettings", &RequestHandler::GetOutputSettings},
	{"SetOutputSettings", &RequestHandler::SetOutputSettings},

	// Stream
	{"GetStreamStatus", &RequestHandler::GetStreamStatus},
	{"ToggleStream", &RequestHandler::ToggleStream},
	{"StartStream", &RequestHandler::StartStream},
	{"StopStream", &RequestHandler::StopStream},
	{"SendStreamCaption", &RequestHandler::SendStreamCaption},

	// Record
	{"GetRecordStatus", &RequestHandler::GetRecordStatus},
	{"ToggleRecord", &RequestHandler::ToggleRecord},
	{"StartRecord", &RequestHandler::StartRecord},
	{"StopRecord", &RequestHandler::StopRecord},
	{"ToggleRecordPause", &RequestHandler::ToggleRecordPause},
	{"PauseRecord", &RequestHandler::PauseRecord},
	{"ResumeRecord", &RequestHandler::ResumeRecord},
	{"SplitRecordFile", &RequestHandler::SplitRecordFile},
	{"CreateRecordChapter", &RequestHandler::CreateRecordChapter},

	// Media Inputs
	{"GetMediaInputStatus", &RequestHandler::GetMediaInputStatus},
	{"SetMediaInputCursor", &RequestHandler::SetMediaInputCursor},
	{"OffsetMediaInputCursor", &RequestHandler::OffsetMediaInputCursor},
	{"TriggerMediaInputAction", &RequestHandler::TriggerMediaInputAction},

	// Ui
	{"GetStudioModeEnabled", &RequestHandler::GetStudioModeEnabled},
	{"SetStudioModeEnabled", &RequestHandler::SetStudioModeEnabled},
	{"OpenInputPropertiesDialog", &RequestHandler::OpenInputPropertiesDialog},
	{"OpenInputFiltersDialog", &RequestHandler::OpenInputFiltersDialog},
	{"OpenInputInteractDialog", &RequestHandler::OpenInputInteractDialog},
	{"GetMonitorList", &RequestHandler::GetMonitorList},
	{"OpenVideoMixProjector", &RequestHandler::OpenVideoMixProjector},
	{"OpenSourceProjector", &RequestHandler::OpenSourceProjector},
};

RequestHandler::RequestHandler(SessionPtr session) : _session(session) {}

RequestResult RequestHandler::ProcessRequest(const Request &request)
{
#ifdef PLUGIN_TESTS
	ScopeProfiler prof{"obs_websocket_request_processing"};
#endif

	if (!request.RequestData.is_object() && !request.RequestData.is_null())
		return RequestResult::Error(RequestStatus::InvalidRequestFieldType, "Your request data is not an object.");

	if (request.RequestType.empty())
		return RequestResult::Error(RequestStatus::MissingRequestType, "Your request's `requestType` may not be empty.");

	RequestMethodHandler handler;
	try {
		handler = _handlerMap.at(request.RequestType);
	} catch (const std::out_of_range &oor) {
		UNUSED_PARAMETER(oor);
		return RequestResult::Error(RequestStatus::UnknownRequestType, "Your request type is not valid.");
	}

	// The graphics and audio threads must never block on the UI thread, and the UI thread can run the request directly
	if (_callingThreadRequests.count(request.RequestType) || obs_in_task_thread(OBS_TASK_UI) ||
	    obs_in_task_thread(OBS_TASK_GRAPHICS) || obs_in_task_thread(OBS_TASK_AUDIO))
		return std::bind(handler, this, std::placeholders::_1)(request);

	// Many obs_frontend_* functions read Qt widgets directly (scene list, transitions, profiles...), so they are only safe
	// on the UI thread. Running the request there also serializes it with user operations in the OBS UI, such as removing
	// a scene or changing the scene collection, which would otherwise free objects while this request is using them.
	struct UiTaskData {
		RequestHandler *self;
		RequestMethodHandler handler;
		const Request *request;
		bool waitForPropertiesDialog;
		bool deferred;
		RequestResult result;
	} data{this, handler, &request, _propertiesRebuildingRequests.count(request.RequestType) > 0, false, {}};

	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	while (true) {
		data.deferred = false;
		obs_queue_task(
			OBS_TASK_UI,
			[](void *param) {
				auto data = static_cast<UiTaskData *>(param);

				// OBS may have become not ready (scene collection change, shutdown) while this task was queued
				auto eventHandler = GetEventHandler();
				if (!eventHandler || !eventHandler->IsObsReady()) {
					data->result = RequestResult::Error(RequestStatus::NotReady,
									    "OBS is not ready to perform the request.");
					return;
				}

				if (data->waitForPropertiesDialog && IsPropertiesDialogOpen()) {
					data->deferred = true;
					return;
				}

				data->result = std::bind(data->handler, data->self, std::placeholders::_1)(*data->request);
			},
			&data, true);

		if (!data.deferred)
			return data.result;

		if (std::chrono::steady_clock::now() >= deadline)
			return RequestResult::Error(
				RequestStatus::NotReady,
				"A dialog is open in an OBS properties or filters window. Close it and try again.");

		// Wait outside of the UI thread so the user can finish with the dialog
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
}

std::vector<std::string> RequestHandler::GetRequestList()
{
	std::vector<std::string> ret;
	for (auto const &[key, val] : _handlerMap)
		ret.push_back(key);

	return ret;
}
