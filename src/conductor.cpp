#include "conductor.h"

#include <api/audio_codecs/builtin_audio_decoder_factory.h>
#include <api/audio_codecs/builtin_audio_encoder_factory.h>
#include <api/create_peerconnection_factory.h>
#include <api/video_codecs/builtin_video_decoder_factory.h>
#include <api/video_codecs/builtin_video_encoder_factory.h>
#include <modules/audio_device/include/audio_device.h>
#include <modules/audio_device/include/audio_device_factory.h>
#include <modules/audio_device/linux/audio_device_alsa_linux.h>
#include <modules/audio_device/linux/audio_device_pulse_linux.h>
#include <modules/audio_device/linux/latebindingsymboltable_linux.h>
#include <rtc_base/ssl_adapter.h>
#include <rtc_base/thread.h>

Conductor::Conductor(std::string signal_url) : signalr_url(signalr_url)
{
    std::cout << "=> Conductor: init" << std::endl;
    if (InitializePeerConnection())
    {
        std::cout << "=> InitializePeerConnection: success, start connecting signaling server!" << std::endl;
    }
}

void Conductor::ConnectToPeer()
{
    std::cout << "=> ConnectToPeer: " << (peer_connection_ != nullptr) << std::endl;
    // peer_connection_->CreateOffer(this, webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
}

void Conductor::AddTracks()
{
    if (!peer_connection_->GetSenders().empty())
    {
        std::cout << "=> AddTracks: already add tracks." << std::endl;
        return;
    }

    auto options = peer_connection_factory_->CreateAudioSource(cricket::AudioOptions());
    audio_track_ = peer_connection_factory_->CreateAudioTrack(
        "my_audio_label", options.get());

    auto res = peer_connection_->AddTrack(audio_track_, {"my_stream_id"});
    if (!res.ok())
    {
        std::cout << "=> AddTracks: audio_track_ " << res.error().message() << std::endl;
    }
    else
    {
        std::cout << "=> AddTracks: audio_track_ success!" << std::endl;
    }
}

bool Conductor::CreatePeerConnection()
{
    webrtc::PeerConnectionInterface::RTCConfiguration config;
    config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
    webrtc::PeerConnectionInterface::IceServer server;
    server.uri = "stun:stun.l.google.com:19302";
    config.servers.push_back(server);

    peer_connection_ =
        peer_connection_factory_->CreatePeerConnection(config, nullptr, nullptr, this);
    return peer_connection_ != nullptr;
}

bool Conductor::InitializePeerConnection()
{
    rtc::InitializeSSL();

    network_thread_ = rtc::Thread::CreateWithSocketServer();
    worker_thread_ = rtc::Thread::Create();
    signaling_thread_ = rtc::Thread::Create();

    if (network_thread_->Start())
    {
        std::cout << "=> network thread start: success!" << std::endl;
    }
    if (worker_thread_->Start())
    {
        std::cout << "=> worker thread start: success!" << std::endl;
    }
    if (signaling_thread_->Start())
    {
        std::cout << "=> signaling thread start: success!" << std::endl;
    }

    peer_connection_factory_ = webrtc::CreatePeerConnectionFactory(
        network_thread_.get(), worker_thread_.get(), signaling_thread_.get(), nullptr,
        webrtc::CreateBuiltinAudioEncoderFactory(), webrtc::CreateBuiltinAudioDecoderFactory(),
        webrtc::CreateBuiltinVideoEncoderFactory(), webrtc::CreateBuiltinVideoDecoderFactory(),
        nullptr, nullptr);

    if (!peer_connection_factory_)
    {
        std::cout << "=> peer_connection_factory: failed" << std::endl;
    }
    else
    {
        std::cout << "=> peer_connection_factory: success!" << std::endl;
    }

    if (CreatePeerConnection())
    {
        std::cout << "=> create peer connection: success!" << std::endl;
    }

    AddTracks();

    return peer_connection_ != nullptr;
}

void Conductor::OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState new_state)
{
    std::cout << "=> OnSignalingChange: " << new_state << std::endl;
}

void Conductor::OnDataChannel(rtc::scoped_refptr<webrtc::DataChannelInterface> channel)
{
    channel_=channel;
    std::cout << "=> OnDataChannel: connected to " << channel->label() <<", "<< channel->state() << std::endl;
}

void Conductor::SendData(const std::string msg){
    if (channel_->state() != webrtc::DataChannelInterface::kOpen) {
        std::cout << "=> channel: != kopen " << std::endl;
        return;
    }
    webrtc::DataBuffer data(msg);
    channel_->Send(data);
    std::cout << "=> SendData: \"" << msg << "\" was sent to " << channel_->label() << std::endl;
}

void Conductor::OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState new_state)
{
    std::cout << "=> OnConnectionChange: " << webrtc::PeerConnectionInterface::PeerConnectionInterface::AsString(new_state) << std::endl;
}

void Conductor::OnStandardizedIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState new_state)
{
    std::cout << "=> OnIceGatheringChange: " << new_state << std::endl;
}

void Conductor::OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState new_state)
{
    std::cout << "=> OnIceGatheringChange: " << new_state << std::endl;
}

void Conductor::OnIceCandidate(const webrtc::IceCandidateInterface *candidate)
{
    std::string ice;
    candidate->ToString(&ice);
    invoke_answer_ice_(candidate->sdp_mid(), candidate->sdp_mline_index(), ice);
}

void Conductor::SetOfferSDP(const std::string sdp,
                            OnSetSuccessFunc on_success,
                            OnFailureFunc on_failure)
{
    std::cout << "=> SetOfferSDP: start" << std::endl;

    webrtc::SdpParseError error;
    std::unique_ptr<webrtc::SessionDescriptionInterface> session_description =
        webrtc::CreateSessionDescription(webrtc::SdpType::kOffer, sdp, &error);
    if (!session_description)
    {
        RTC_LOG(LS_ERROR) << __FUNCTION__
                          << "Failed to create session description: "
                          << error.description.c_str()
                          << "\nline: " << error.line.c_str();
        return;
    }
    peer_connection_->SetRemoteDescription(
        SetSessionDescription::Create(std::move(on_success), std::move(on_failure)),
        session_description.release());
}

void Conductor::AddIceCandidate(std::string sdp_mid, int sdp_mline_index, std::string sdp)
{
    std::cout << "=> AddIceCandidate: start creating" << std::endl;
    webrtc::SdpParseError error;
    std::unique_ptr<webrtc::IceCandidateInterface> candidate(
        webrtc::CreateIceCandidate(sdp_mid, sdp_mline_index, sdp, &error));
    if (!candidate.get())
    {
        RTC_LOG(LS_ERROR) << "Can't parse received candidate message: "
                          << error.description.c_str()
                          << "\nline: " << error.line.c_str();
        return;
    }
    std::cout << "=> AddIceCandidate: add candidated" << std::endl;
    peer_connection_->AddIceCandidate(
        std::move(candidate), [sdp](webrtc::RTCError error)
        { RTC_LOG(LS_WARNING)
              << __FUNCTION__ << " Failed to apply the received candidate. type="
              << webrtc::ToString(error.type()) << " message=" << error.message()
              << " sdp=" << sdp; });
    std::cout << "=> AddIceCandidate: end" << std::endl;
}

void Conductor::CreateAnswer(OnCreateSuccessFunc on_success, OnFailureFunc on_failure)
{
    auto with_set_local_desc = [this, on_success = std::move(on_success)](
                                   webrtc::SessionDescriptionInterface *desc)
    {
        std::string sdp;
        desc->ToString(&sdp);
        peer_connection_->SetLocalDescription(
            SetSessionDescription::Create(nullptr, nullptr), desc);
        if (on_success)
        {
            on_success(desc);
        }
    };
    peer_connection_->CreateAnswer(
        CreateSessionDescription::Create(std::move(with_set_local_desc), std::move(on_failure)),
        webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
}

Conductor::~Conductor()
{
    // network_thread_->Stop();
    // worker_thread_->Stop();
    // signaling_thread_->Stop();
    // audio_track_ = nullptr;
    // video_track_ = nullptr;
    // peer_connection_factory_ = nullptr;
    rtc::CleanupSSL();
    std::cout << "=> ~Conductor: destroied" << std::endl;
}
