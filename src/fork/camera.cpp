/**
 * @file src/fork/camera.cpp
 * @brief Fork-only: LuzLunar's Mac camera as this PC's "Mac Camera" (camera.h).
 */
// standard includes
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// lib includes
#include <boost/asio.hpp>

// local includes
#include "camera.h"
#include "camera_policy.h"
#include "fork_config.h"
#include "src/logging.h"
#include "src/nvhttp.h"
#include "src/rtsp.h"
#include "src/stream.h"
#include "src/utility.h"
#include "user_helper.h"

#ifdef _WIN32
  // platform includes
  #include <windows.h>
  #include <aclapi.h>
  #include <mfapi.h>
  #include <mferror.h>
  #include <mfidl.h>
  #include <mftransform.h>
  #include <sddl.h>
  #include <strmif.h>
  #include <wrl/client.h>

  #include "src/platform/windows/misc.h"
#endif

using namespace std::literals;

namespace fork_tweaks::camera {
#ifdef _WIN32
  namespace {
    using clock = std::chrono::steady_clock;
    using Microsoft::WRL::ComPtr;
    namespace shared = camera_policy::shared;

    std::atomic_bool g_supported {false};

    // CODECAPI_AVLowLatencyMode, which MinGW's codecapi.h only names as a __uuidof
    constexpr GUID k_low_latency_mode = {0x9c27891a, 0xed7a, 0x40e1, {0x88, 0xe8, 0xb2, 0x27, 0x27, 0xa0, 0x24, 0xee}};

    std::string narrow(const std::wstring &wide) {
      if (wide.empty()) {
        return {};
      }
      int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int) wide.size(), nullptr, 0, nullptr, nullptr);
      std::string out(size > 0 ? size : 0, '\0');
      if (size > 0) {
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int) wide.size(), out.data(), size, nullptr, nullptr);
      }
      return out;
    }

    std::wstring widen(const std::string &text) {
      if (text.empty()) {
        return {};
      }
      int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), (int) text.size(), nullptr, 0);
      std::wstring out(size > 0 ? size : 0, L'\0');
      if (size > 0) {
        MultiByteToWideChar(CP_UTF8, 0, text.data(), (int) text.size(), out.data(), size);
      }
      return out;
    }

    /**
     * @brief One client's camera connection. Its I/O runs on nvhttp's thread, which every handler
     * here runs on; send() and close() hand work to it from any thread. Pending operations hold it,
     * so it's freed once it's closed and they've finished.
     */
    class connection_t: public std::enable_shared_from_this<connection_t> {
    public:
      connection_t(std::unique_ptr<nvhttp::SunshineHTTPS> socket, request_t request):
          m_Socket(std::move(socket)),
          m_Executor(m_Socket->get_executor()),
          m_Request(std::move(request)),
          m_Timer(m_Executor),
          m_Id(s_NextId.fetch_add(1) + 1) {
      }

      ~connection_t() {
        // A closed socket's TLS shutdown (the socket's destructor) fails at once, rather than waiting
        // for a peer that may be gone
        boost::system::error_code ignored;
        m_Socket->lowest_layer().close(ignored);
      }

      connection_t(const connection_t &) = delete;
      connection_t &operator=(const connection_t &) = delete;

      /// Answers with status and reason, and closes once that's written
      void refuse(int status, std::string_view reason) {
        std::string_view text = status == 401 ? "Unauthorized"sv : status == 403 ? "Forbidden"sv :
                                status == 409 ? "Conflict"sv :
                                                "Not Found"sv;
        std::string response = "HTTP/1.1 "s + std::to_string(status) + " " + std::string(text) +
                               "\r\nContent-Type: text/plain\r\nContent-Length: " + std::to_string(reason.size()) +
                               "\r\nConnection: close\r\n\r\n" + std::string(reason);
        m_CloseAfterWrite = true;
        queue(std::vector<std::uint8_t>(response.begin(), response.end()));
      }

      /// Answers 101, then speaks luzcamera.h's protocol until the stream or the connection ends
      void accept(std::weak_ptr<stream::session_t> session);

      /// Queues a message for the Mac, from any thread
      void send(std::vector<std::uint8_t> message) {
        boost::asio::post(m_Executor, [self = shared_from_this(), message = std::move(message)]() mutable {
          if (!self->m_Closed) {
            self->queue(std::move(message));
          }
        });
      }

      /// Closes the connection, from any thread
      void close(std::string why) {
        boost::asio::post(m_Executor, [self = shared_from_this(), why = std::move(why)]() {
          self->shut(why);
        });
      }

      const request_t &request() const {
        return m_Request;
      }

      std::uint64_t id() const {
        return m_Id;
      }

    private:
      void queue(std::vector<std::uint8_t> bytes) {
        m_Writes.push_back(std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes)));
        if (!m_Writing) {
          write_next();
        }
      }

      void write_next() {
        if (m_Writes.empty() || m_Closed) {
          m_Writing = false;
          if (m_CloseAfterWrite && !m_Closed) {
            shut("refused");
          }
          return;
        }
        m_Writing = true;
        auto bytes = m_Writes.front();
        boost::asio::async_write(*m_Socket, boost::asio::buffer(*bytes), [self = shared_from_this(), bytes](const boost::system::error_code &ec, std::size_t) {
          if (ec) {
            self->shut("couldn't send: "s + ec.message());
            return;
          }
          self->m_Writes.pop_front();
          self->write_next();
        });
      }

      void read() {
        m_Socket->async_read_some(boost::asio::buffer(m_ReadBuffer), [self = shared_from_this()](const boost::system::error_code &ec, std::size_t bytes) {
          self->on_read(ec, bytes);
        });
      }

      void on_read(const boost::system::error_code &ec, std::size_t bytes);

      void tick();

      void shut(const std::string &why);

      static inline std::atomic<std::uint64_t> s_NextId {0};

      std::unique_ptr<nvhttp::SunshineHTTPS> m_Socket;
      boost::asio::any_io_executor m_Executor;
      request_t m_Request;
      boost::asio::steady_timer m_Timer;
      std::uint64_t m_Id;
      std::weak_ptr<stream::session_t> m_Session;
      std::array<std::uint8_t, 64 * 1024> m_ReadBuffer {};
      luzcamera::reader_t m_Reader;
      std::deque<std::shared_ptr<const std::vector<std::uint8_t>>> m_Writes;
      bool m_Writing = false;
      bool m_Closed = false;
      bool m_CloseAfterWrite = false;
      bool m_Accepted = false;
      clock::time_point m_LastHeard;
    };

    /// A frame from the Mac, for the decoder
    struct frame_t {
      bool keyframe = false;
      camera_policy::frame_size_t size;
      std::uint64_t time_us = 0;
      std::vector<std::uint8_t> au;
    };

    /// Media Foundation's H.264 decoder in low-latency mode, decoding to NV12 on the CPU
    class decoder_t {
    public:
      ~decoder_t() {
        close();
      }

      bool open(camera_policy::frame_size_t size) {
        close();
        MFT_REGISTER_TYPE_INFO input {MFMediaType_Video, MFVideoFormat_H264};
        MFT_REGISTER_TYPE_INFO output {MFMediaType_Video, MFVideoFormat_NV12};
        IMFActivate **activates = nullptr;
        UINT32 count = 0;
        HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER, &input, &output, &activates, &count);
        if (FAILED(hr) || count == 0) {
          BOOST_LOG(error) << "fork: camera: no H.264 decoder in Media Foundation: 0x"sv << util::hex(hr).to_string_view();
          return false;
        }
        for (UINT32 i = 0; i < count; i++) {
          if (!m_Mft) {
            activates[i]->ActivateObject(IID_PPV_ARGS(m_Mft.ReleaseAndGetAddressOf()));
          }
          activates[i]->Release();
        }
        CoTaskMemFree(activates);
        if (!m_Mft) {
          BOOST_LOG(error) << "fork: camera: couldn't start the H.264 decoder"sv;
          return false;
        }

        // Each frame out as soon as it's in: there's no reordering to wait for
        ComPtr<IMFAttributes> attributes;
        if (SUCCEEDED(m_Mft->GetAttributes(&attributes)) && attributes) {
          attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
        }
        ComPtr<ICodecAPI> codec;
        if (SUCCEEDED(m_Mft->QueryInterface(IID_ICodecAPI, reinterpret_cast<void **>(codec.GetAddressOf())))) {
          VARIANT on {};
          on.vt = VT_UI4;
          on.ulVal = 1;
          codec->SetValue(&k_low_latency_mode, &on);
        }

        ComPtr<IMFMediaType> type;
        if (FAILED(MFCreateMediaType(&type))) {
          close();
          return false;
        }
        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, size.width, size.height);
        MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, camera_policy::k_fps, 1);
        type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        hr = m_Mft->SetInputType(0, type.Get(), 0);
        if (FAILED(hr)) {
          BOOST_LOG(error) << "fork: camera: the H.264 decoder refused "sv << size.width << 'x' << size.height << ": 0x"sv << util::hex(hr).to_string_view();
          close();
          return false;
        }
        if (!set_output_type()) {
          BOOST_LOG(error) << "fork: camera: the H.264 decoder offers no NV12"sv;
          close();
          return false;
        }
        m_Mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        m_Mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        m_Size = size;
        m_Packed.resize(camera_policy::nv12_bytes(size.width, size.height));
        return true;
      }

      void close() {
        if (m_Mft) {
          m_Mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
          m_Mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        }
        m_Mft.Reset();
        m_Size = {};
      }

      bool is_open(camera_policy::frame_size_t size) const {
        return m_Mft && m_Size == size;
      }

      /// Decodes one access unit; out(nv12, size) gets each frame that comes out, tightly packed
      template<typename F>
      bool decode(const frame_t &frame, F &&out) {
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(MFCreateMemoryBuffer((DWORD) frame.au.size(), &buffer))) {
          return false;
        }
        BYTE *data = nullptr;
        if (FAILED(buffer->Lock(&data, nullptr, nullptr))) {
          return false;
        }
        std::memcpy(data, frame.au.data(), frame.au.size());
        buffer->Unlock();
        buffer->SetCurrentLength((DWORD) frame.au.size());

        ComPtr<IMFSample> sample;
        if (FAILED(MFCreateSample(&sample))) {
          return false;
        }
        sample->AddBuffer(buffer.Get());
        sample->SetSampleTime((LONGLONG) (frame.time_us * 10));
        sample->SetSampleDuration(10'000'000 / camera_policy::k_fps);
        if (frame.keyframe) {
          sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
        }

        HRESULT hr = m_Mft->ProcessInput(0, sample.Get(), 0);
        if (hr == MF_E_NOTACCEPTING) {
          if (!drain(out)) {
            return false;
          }
          hr = m_Mft->ProcessInput(0, sample.Get(), 0);
        }
        if (FAILED(hr)) {
          BOOST_LOG(warning) << "fork: camera: the decoder refused a frame: 0x"sv << util::hex(hr).to_string_view();
          return false;
        }
        return drain(out);
      }

    private:
      bool set_output_type() {
        for (DWORD i = 0;; i++) {
          ComPtr<IMFMediaType> type;
          if (FAILED(m_Mft->GetOutputAvailableType(0, i, &type))) {
            return false;
          }
          GUID subtype {};
          if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) || subtype != MFVideoFormat_NV12) {
            continue;
          }
          if (FAILED(m_Mft->SetOutputType(0, type.Get(), 0))) {
            return false;
          }
          UINT32 width = 0;
          UINT32 height = 0;
          MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width, &height);
          auto stride = (INT32) MFGetAttributeUINT32(type.Get(), MF_MT_DEFAULT_STRIDE, width);
          m_OutWidth = width;
          m_OutHeight = height;
          m_Stride = stride > 0 ? (std::uint32_t) stride : width;
          MFT_OUTPUT_STREAM_INFO info {};
          m_Mft->GetOutputStreamInfo(0, &info);
          m_ProvidesSamples = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
          m_OutputBytes = std::max<DWORD>(info.cbSize, (DWORD) ((std::size_t) m_Stride * m_OutHeight * 3 / 2));
          return true;
        }
      }

      template<typename F>
      bool drain(F &&out) {
        for (;;) {
          MFT_OUTPUT_DATA_BUFFER output {};
          output.dwStreamID = 0;
          ComPtr<IMFSample> mine;
          if (!m_ProvidesSamples) {
            ComPtr<IMFMediaBuffer> buffer;
            if (FAILED(MFCreateSample(&mine)) || FAILED(MFCreateMemoryBuffer(m_OutputBytes, &buffer))) {
              return false;
            }
            mine->AddBuffer(buffer.Get());
            output.pSample = mine.Get();
          }
          DWORD status = 0;
          HRESULT hr = m_Mft->ProcessOutput(0, 1, &output, &status);
          if (output.pEvents) {
            output.pEvents->Release();
          }
          // The decoder's own sample is ours to release
          ComPtr<IMFSample> got;
          if (output.pSample && !mine) {
            got.Attach(output.pSample);
          } else {
            got = mine;
          }
          if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            return true;
          }
          if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            if (!set_output_type()) {
              return false;
            }
            continue;
          }
          if (FAILED(hr)) {
            BOOST_LOG(warning) << "fork: camera: decoding failed: 0x"sv << util::hex(hr).to_string_view();
            return false;
          }
          if (!got || !copy_out(got.Get())) {
            return false;
          }
          out(m_Packed.data(), m_Size);
        }
      }

      /// The decoded frame's visible part into m_Packed
      bool copy_out(IMFSample *sample) {
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->GetBufferByIndex(0, &buffer))) {
          return false;
        }
        std::uint32_t width = std::min(m_Size.width, m_OutWidth);
        std::uint32_t height = std::min(m_Size.height, m_OutHeight);
        if (width != m_Size.width || height != m_Size.height) {
          return false;
        }
        ComPtr<IMF2DBuffer> two_d;
        if (SUCCEEDED(buffer.As(&two_d))) {
          BYTE *scanline = nullptr;
          LONG pitch = 0;
          if (SUCCEEDED(two_d->Lock2D(&scanline, &pitch))) {
            bool ok = pitch >= (LONG) width;
            if (ok) {
              camera_policy::nv12::crop(scanline, (std::size_t) pitch, (std::size_t) pitch * m_OutHeight, width, height, m_Packed.data());
            }
            two_d->Unlock2D();
            return ok;
          }
        }
        BYTE *data = nullptr;
        DWORD length = 0;
        if (FAILED(buffer->Lock(&data, nullptr, &length))) {
          return false;
        }
        std::size_t needed = (std::size_t) m_Stride * m_OutHeight + (std::size_t) m_Stride * ((height + 1) / 2);
        bool ok = length >= needed && m_Stride >= width;
        if (ok) {
          camera_policy::nv12::crop(data, m_Stride, (std::size_t) m_Stride * m_OutHeight, width, height, m_Packed.data());
        }
        buffer->Unlock();
        return ok;
      }

      ComPtr<IMFTransform> m_Mft;
      camera_policy::frame_size_t m_Size;
      std::uint32_t m_OutWidth = 0;
      std::uint32_t m_OutHeight = 0;
      std::uint32_t m_Stride = 0;
      DWORD m_OutputBytes = 0;
      bool m_ProvidesSamples = false;
      std::vector<std::uint8_t> m_Packed;
    };

    /// The placeholder: the camera icon above a line of text, drawn with GDI into NV12
    std::vector<std::uint8_t> render_placeholder(camera_policy::frame_size_t size, const std::string &text) {
      namespace nv12 = camera_policy::nv12;
      std::vector<std::uint8_t> frame(camera_policy::nv12_bytes(size.width, size.height));
      nv12::fill(frame.data(), size.width, size.height, size.width, nv12::k_background);

      HDC dc = CreateCompatibleDC(nullptr);
      BITMAPINFO info {};
      info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
      info.bmiHeader.biWidth = (LONG) size.width;
      info.bmiHeader.biHeight = -(LONG) size.height;  // top-down
      info.bmiHeader.biPlanes = 1;
      info.bmiHeader.biBitCount = 32;
      info.bmiHeader.biCompression = BI_RGB;
      void *bits = nullptr;
      HBITMAP bitmap = dc ? CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
      if (bitmap && bits) {
        HGDIOBJ old_bitmap = SelectObject(dc, bitmap);
        auto *pixels = static_cast<std::uint32_t *>(bits);
        std::fill(pixels, pixels + (std::size_t) size.width * size.height, 0xFF1C1C1Eu);  // the background, as RGB(28, 28, 30)
        HFONT font = CreateFontW(-(int) (size.height / 24), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        HGDIOBJ old_font = font ? SelectObject(dc, font) : nullptr;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(235, 235, 240));
        std::wstring wide = widen(text);
        RECT rect {(LONG) (size.width / 8), (LONG) (size.height * 0.60), (LONG) (size.width - size.width / 8), (LONG) (size.height * 0.94)};
        DrawTextW(dc, wide.c_str(), (int) wide.size(), &rect, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
        GdiFlush();
        nv12::from_bgra(static_cast<const std::uint8_t *>(bits), (std::size_t) size.width * 4, size.width, size.height, frame.data());
        if (old_font) {
          SelectObject(dc, old_font);
        }
        if (font) {
          DeleteObject(font);
        }
        SelectObject(dc, old_bitmap);
      }
      if (bitmap) {
        DeleteObject(bitmap);
      }
      if (dc) {
        DeleteDC(dc);
      }
      nv12::draw_camera_icon(frame.data(), size.width, size.height, size.width);
      return frame;
    }

    /// The SYSTEM SID, to tell Apollo's own objects (kept open by Frame Server across a restart) from others
    bool owned_by_system(HANDLE object) {
      PSID owner = nullptr;
      PSECURITY_DESCRIPTOR descriptor = nullptr;
      if (GetSecurityInfo(object, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &descriptor) != ERROR_SUCCESS) {
        return false;
      }
      BYTE buffer[SECURITY_MAX_SID_SIZE];
      DWORD size = sizeof(buffer);
      bool system = CreateWellKnownSid(WinLocalSystemSid, nullptr, buffer, &size) && owner && EqualSid(owner, buffer);
      LocalFree(descriptor);
      return system;
    }

    /// A Global object with its security descriptor; one that exists already must be SYSTEM's
    template<typename Create>
    HANDLE make_object(const wchar_t *name, const wchar_t *sddl, Create &&create) {
      PSECURITY_DESCRIPTOR descriptor = nullptr;
      if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &descriptor, nullptr)) {
        BOOST_LOG(error) << "fork: camera: bad security descriptor for "sv << narrow(name) << ": error "sv << GetLastError();
        return nullptr;
      }
      SECURITY_ATTRIBUTES attributes {sizeof(attributes), descriptor, FALSE};
      HANDLE handle = create(&attributes);
      DWORD made = GetLastError();
      LocalFree(descriptor);
      if (!handle) {
        BOOST_LOG(error) << "fork: camera: couldn't make "sv << narrow(name) << ": error "sv << made;
        return nullptr;
      }
      if (made == ERROR_ALREADY_EXISTS && !owned_by_system(handle)) {
        BOOST_LOG(error) << "fork: camera: "sv << narrow(name) << " exists already, made by someone else"sv;
        CloseHandle(handle);
        return nullptr;
      }
      return handle;
    }

    /**
     * @brief The camera's thread and what it shares: the shared memory and events with the media
     * source, and what the connection hands it (thread-safe).
     */
    class service_t {
    public:
      bool open() {
        m_FramesMap = make_object(shared::k_frames_name, shared::k_frames_sddl, [](SECURITY_ATTRIBUTES *attributes) {
          return CreateFileMappingW(INVALID_HANDLE_VALUE, attributes, PAGE_READWRITE, (DWORD) ((std::uint64_t) shared::k_frames_bytes >> 32), (DWORD) shared::k_frames_bytes, shared::k_frames_name);
        });
        bool frames_existed = GetLastError() == ERROR_ALREADY_EXISTS;
        m_ControlMap = make_object(shared::k_control_name, shared::k_control_sddl, [](SECURITY_ATTRIBUTES *attributes) {
          return CreateFileMappingW(INVALID_HANDLE_VALUE, attributes, PAGE_READWRITE, 0, (DWORD) shared::k_control_bytes, shared::k_control_name);
        });
        m_FrameEvent = make_object(shared::k_frame_event_name, shared::k_frame_event_sddl, [](SECURITY_ATTRIBUTES *attributes) {
          return CreateEventW(attributes, FALSE, FALSE, shared::k_frame_event_name);
        });
        m_StateEvent = make_object(shared::k_state_event_name, shared::k_state_event_sddl, [](SECURITY_ATTRIBUTES *attributes) {
          return CreateEventW(attributes, FALSE, FALSE, shared::k_state_event_name);
        });
        m_Wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!m_FramesMap || !m_ControlMap || !m_FrameEvent || !m_StateEvent || !m_Wake) {
          close();
          return false;
        }
        m_Frames = static_cast<std::uint8_t *>(MapViewOfFile(m_FramesMap, FILE_MAP_ALL_ACCESS, 0, 0, shared::k_frames_bytes));
        m_Control = static_cast<std::uint8_t *>(MapViewOfFile(m_ControlMap, FILE_MAP_ALL_ACCESS, 0, 0, shared::k_control_bytes));
        if (!m_Frames || !m_Control) {
          BOOST_LOG(error) << "fork: camera: couldn't map the shared memory: error "sv << GetLastError();
          close();
          return false;
        }
        // A section Frame Server kept open across a restart keeps its frames; the control block's
        // contents are the source's, and stay
        if (!frames_existed || !shared::frames_valid(m_Frames)) {
          shared::init_frames(m_Frames);
        }
        shared::init_control(m_Control);
        shared::beat_frames(m_Frames, GetTickCount64());
        return true;
      }

      void close() {
        if (m_Frames) {
          UnmapViewOfFile(m_Frames);
          m_Frames = nullptr;
        }
        if (m_Control) {
          UnmapViewOfFile(m_Control);
          m_Control = nullptr;
        }
        for (HANDLE *handle : {&m_FramesMap, &m_ControlMap, &m_FrameEvent, &m_StateEvent, &m_Wake}) {
          if (*handle) {
            CloseHandle(*handle);
            *handle = nullptr;
          }
        }
      }

      void run_async() {
        m_Thread = std::thread([this]() {
          run();
        });
      }

      void stop() {
        m_Stop = true;
        if (m_Wake) {
          SetEvent(m_Wake);
        }
        if (m_Thread.joinable()) {
          m_Thread.join();
        }
        close();
      }

      /// A client's connection takes the camera; false while another client's has it
      bool attach(const std::shared_ptr<connection_t> &connection) {
        std::lock_guard lock {m_Lock};
        if (auto current = m_Connection.lock()) {
          if (current->request().client_uuid != connection->request().client_uuid) {
            return false;
          }
          // The same client again: the old connection is stale
          current->close("replaced by a new connection from the same client");
        }
        m_Connection = connection;
        m_ConnectionId = connection->id();
        m_MacName = connection->request().client_name;
        m_Status.reset();
        m_Inbox.clear();
        m_ConnectionChanged = true;
        SetEvent(m_Wake);
        return true;
      }

      void hello(std::uint64_t id, const std::string &name) {
        std::lock_guard lock {m_Lock};
        if (id == m_ConnectionId && !name.empty()) {
          m_MacName = name;
          SetEvent(m_Wake);
        }
      }

      void video(std::uint64_t id, const luzcamera::video_t &video) {
        std::lock_guard lock {m_Lock};
        if (id != m_ConnectionId) {
          return;
        }
        if (m_Inbox.size() >= camera_policy::max_queued_frames) {
          // The decoder can't keep up: start again from a keyframe
          m_Inbox.clear();
          m_Overflow = true;
        }
        frame_t frame;
        frame.keyframe = video.keyframe;
        frame.size = {video.width, video.height};
        frame.time_us = video.time_us;
        frame.au.assign(video.data, video.data + video.size);
        m_Inbox.push_back(std::move(frame));
        SetEvent(m_Wake);
      }

      void status(std::uint64_t id, const luzcamera::status_t &status) {
        std::lock_guard lock {m_Lock};
        if (id == m_ConnectionId) {
          m_Status = status;
          SetEvent(m_Wake);
        }
      }

      void closed(std::uint64_t id) {
        std::lock_guard lock {m_Lock};
        if (id == m_ConnectionId) {
          m_Connection.reset();
          m_ConnectionId = 0;
          m_Inbox.clear();
          m_Status.reset();
          m_ConnectionChanged = true;
          SetEvent(m_Wake);
        }
      }

    private:
      void run();
      std::optional<std::string> camera_stream_client();
      void show_placeholder(camera_policy::frame_size_t size, const std::string &text);

      std::thread m_Thread;
      std::atomic_bool m_Stop {false};
      HANDLE m_FramesMap = nullptr;
      HANDLE m_ControlMap = nullptr;
      HANDLE m_FrameEvent = nullptr;
      HANDLE m_StateEvent = nullptr;
      HANDLE m_Wake = nullptr;
      std::uint8_t *m_Frames = nullptr;
      std::uint8_t *m_Control = nullptr;

      // What the connection hands over, under m_Lock
      std::mutex m_Lock;
      std::weak_ptr<connection_t> m_Connection;
      std::uint64_t m_ConnectionId = 0;
      std::string m_MacName;
      std::optional<luzcamera::status_t> m_Status;
      std::deque<frame_t> m_Inbox;
      bool m_Overflow = false;
      bool m_ConnectionChanged = false;

      // The placeholder shown last ("" after a frame from the Mac), on the camera's thread
      std::string m_ShownPlaceholder;
    };

    std::unique_ptr<service_t> g_service;

    /// Registers the camera with Windows, or removes it, beside the camera's thread
    std::thread g_registrar;

    void connection_t::accept(std::weak_ptr<stream::session_t> session) {
      m_Session = std::move(session);
      m_Accepted = true;
      m_LastHeard = clock::now();
      std::string response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: "s + luzcamera::k_upgrade + "\r\nConnection: Upgrade\r\n\r\n";
      queue(std::vector<std::uint8_t>(response.begin(), response.end()));
      read();
      tick();
    }

    void connection_t::on_read(const boost::system::error_code &ec, std::size_t bytes) {
      if (m_Closed) {
        return;
      }
      if (ec) {
        shut(ec == boost::asio::error::eof || ec == boost::asio::ssl::error::stream_truncated ? "closed by the client"s : "couldn't read: "s + ec.message());
        return;
      }
      m_LastHeard = clock::now();
      m_Reader.feed(m_ReadBuffer.data(), bytes);
      luzcamera::message_t message;
      for (;;) {
        auto result = m_Reader.next(message);
        if (result == luzcamera::reader_t::result_e::need_more) {
          break;
        }
        if (result == luzcamera::reader_t::result_e::error) {
          shut("a message too large");
          return;
        }
        switch (message.type()) {
          case luzcamera::type_e::hello:
            {
              luzcamera::hello_t hello;
              if (luzcamera::parse_hello(message, hello)) {
                BOOST_LOG(info) << "fork: camera: ["sv << hello.name << "] connected its camera (protocol "sv << hello.version << ')';
                if (g_service) {
                  g_service->hello(m_Id, hello.name);
                }
              }
              break;
            }
          case luzcamera::type_e::video:
            {
              luzcamera::video_t video;
              if (luzcamera::parse_video(message, video) && g_service) {
                g_service->video(m_Id, video);
              }
              break;
            }
          case luzcamera::type_e::status:
            {
              luzcamera::status_t status;
              if (luzcamera::parse_status(message, status)) {
                BOOST_LOG(info) << "fork: camera: ["sv << m_Request.client_name << "] says its camera is "sv
                                << (status.state == luzcamera::status_e::live      ? "live"sv :
                                    status.state == luzcamera::status_e::stopped   ? "stopped"sv :
                                    status.state == luzcamera::status_e::denied    ? "not allowed by macOS"sv :
                                    status.state == luzcamera::status_e::off       ? "off in its settings"sv :
                                                                                     "unavailable"sv)
                                << (status.detail.empty() ? ""s : ": "s + status.detail);
                if (g_service) {
                  g_service->status(m_Id, status);
                }
              }
              break;
            }
          default:
            // PING, and anything newer: hearing it is enough
            break;
        }
      }
      read();
    }

    void connection_t::tick() {
      if (m_Closed) {
        return;
      }
      auto now = clock::now();
      if (now - m_LastHeard > std::chrono::milliseconds(luzcamera::k_silence_timeout_ms)) {
        shut("nothing heard for "s + std::to_string(luzcamera::k_silence_timeout_ms / 1000) + " s");
        return;
      }
      auto session = m_Session.lock();
      if (!session || stream::session::state(*session) == stream::session::state_e::STOPPING || stream::session::state(*session) == stream::session::state_e::STOPPED) {
        shut("the stream ended");
        return;
      }
      queue(luzcamera::make_ping((std::uint64_t) std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()));
      m_Timer.expires_after(std::chrono::milliseconds(luzcamera::k_ping_interval_ms));
      m_Timer.async_wait([self = shared_from_this()](const boost::system::error_code &ec) {
        if (!ec) {
          self->tick();
        }
      });
    }

    void connection_t::shut(const std::string &why) {
      if (m_Closed) {
        return;
      }
      m_Closed = true;
      if (m_Accepted) {
        BOOST_LOG(info) << "fork: camera: ["sv << m_Request.client_name << "]'s camera disconnected: "sv << why;
        if (g_service) {
          g_service->closed(m_Id);
        }
      }
      m_Timer.cancel();
      boost::system::error_code ignored;
      m_Socket->lowest_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
      m_Socket->lowest_layer().close(ignored);
    }

    /// tools\apollo-camera.exe with these arguments, as Apollo's own user; its lines go to the log
    int run_tool(const wchar_t *arguments) {
      auto path = user_helper::path(L"apollo-camera.exe");
      SECURITY_ATTRIBUTES inherit {sizeof(inherit), nullptr, TRUE};
      HANDLE out_read = nullptr;
      HANDLE out_write = nullptr;
      if (!CreatePipe(&out_read, &out_write, &inherit, 0)) {
        BOOST_LOG(error) << "fork: camera: couldn't make a pipe: error "sv << GetLastError();
        return -1;
      }
      SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);

      auto list = platf::allocate_proc_thread_attr_list(1);
      if (!list) {
        CloseHandle(out_read);
        CloseHandle(out_write);
        return -1;
      }
      HANDLE handles[] {out_write};
      UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof(handles), nullptr, nullptr);
      STARTUPINFOEXW startup {};
      startup.StartupInfo.cb = sizeof(startup);
      startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
      startup.StartupInfo.hStdOutput = out_write;
      startup.StartupInfo.hStdError = out_write;
      startup.lpAttributeList = list;
      std::wstring command = L"\""s + path.wstring() + L"\" "s + arguments;
      std::wstring dir = path.parent_path().wstring();
      PROCESS_INFORMATION process {};
      BOOL ok = CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, TRUE, EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr, dir.c_str(), &startup.StartupInfo, &process);
      DWORD launch_error = GetLastError();
      platf::free_proc_thread_attr_list(list);
      CloseHandle(out_write);
      if (!ok) {
        BOOST_LOG(error) << "fork: camera: couldn't start "sv << path.string() << ": error "sv << launch_error;
        CloseHandle(out_read);
        return -1;
      }
      CloseHandle(process.hThread);

      // Its lines, until it ends and the pipe closes
      std::string pending;
      char buffer[512];
      DWORD read = 0;
      while (ReadFile(out_read, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
        pending.append(buffer, read);
        std::size_t end;
        while ((end = pending.find('\n')) != std::string::npos) {
          std::string line = pending.substr(0, end);
          if (!line.empty() && line.back() == '\r') {
            line.pop_back();
          }
          if (!line.empty()) {
            user_helper::log_line("camera"sv, line);
          }
          pending.erase(0, end + 1);
        }
      }
      if (!pending.empty()) {
        user_helper::log_line("camera"sv, pending);
      }
      CloseHandle(out_read);
      DWORD code = 1;
      if (WaitForSingleObject(process.hProcess, 30000) != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 1);
      }
      GetExitCodeProcess(process.hProcess, &code);
      CloseHandle(process.hProcess);
      return (int) code;
    }

    std::optional<std::string> service_t::camera_stream_client() {
      for (const auto &uuid : rtsp_stream::get_all_session_uuids()) {
        auto session = rtsp_stream::find_session(uuid);
        if (session) {
          if (auto name = stream::session::fork_camera_client(*session)) {
            return name;
          }
        }
      }
      return std::nullopt;
    }

    void service_t::show_placeholder(camera_policy::frame_size_t size, const std::string &text) {
      std::string key = std::to_string(size.width) + 'x' + std::to_string(size.height) + ' ' + text;
      if (key == m_ShownPlaceholder) {
        return;
      }
      auto frame = render_placeholder(size, text);
      auto now_us = (std::uint64_t) std::chrono::duration_cast<std::chrono::microseconds>(clock::now().time_since_epoch()).count();
      if (shared::publish(m_Frames, size.width, size.height, shared::kind_e::placeholder, now_us, frame.data())) {
        SetEvent(m_FrameEvent);
        m_ShownPlaceholder = key;
      }
    }

    void service_t::run() {
      HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
      bool mf = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
      if (!mf) {
        BOOST_LOG(error) << "fork: camera: Media Foundation didn't start; the camera shows its placeholder only"sv;
      }

      camera_policy::start_stop_t policy;
      camera_policy::frame_gate_t gate;
      decoder_t decoder;
      bool decoder_failed = false;
      std::optional<clock::time_point> last_camera_frame;
      bool apps_were_streaming = false;
      camera_policy::frame_size_t last_size = camera_policy::k_sizes[0];
      std::uint64_t last_beat = 0;

      while (!m_Stop) {
        DWORD timeout = 250;
        if (auto deadline = policy.deadline()) {
          auto left = std::chrono::duration_cast<std::chrono::milliseconds>(*deadline - clock::now()).count();
          timeout = (DWORD) std::clamp<long long>(left + 1, 1, 250);
        }
        HANDLE handles[] {m_StateEvent, m_Wake};
        WaitForMultipleObjects(2, handles, FALSE, timeout);
        if (m_Stop) {
          break;
        }
        auto now = clock::now();
        std::uint64_t tick = GetTickCount64();
        if (tick - last_beat >= camera_policy::heartbeat_interval_ms) {
          shared::beat_frames(m_Frames, tick);
          last_beat = tick;
        }
        shared::apps_t apps = shared::read_control(m_Control, tick);

        // What the connection handed over
        std::deque<frame_t> frames;
        bool overflow;
        bool changed;
        std::shared_ptr<connection_t> connection;
        std::string mac_name;
        std::optional<luzcamera::status_t> status;
        {
          std::lock_guard lock {m_Lock};
          frames.swap(m_Inbox);
          overflow = std::exchange(m_Overflow, false);
          changed = std::exchange(m_ConnectionChanged, false);
          connection = m_Connection.lock();
          mac_name = m_MacName;
          status = m_Status;
        }
        if (changed) {
          decoder.close();
          decoder_failed = false;
          last_camera_frame.reset();
          gate.reset(now, false);
        }

        if (apps.streaming != apps_were_streaming) {
          BOOST_LOG(info) << "fork: camera: "sv << (apps.streaming ? "an app opened Mac Camera at "s + std::to_string(apps.size.width) + 'x' + std::to_string(apps.size.height) : "the last app closed Mac Camera"s);
          // Frames were dropped while no app watched: decoding starts again from a keyframe
          if (apps.streaming) {
            gate.reset(now, true);
          }
        }

        switch (policy.update(apps.streaming, apps.size, connection != nullptr, now)) {
          case camera_policy::start_stop_t::action_e::start:
            {
              auto start = policy.start();
              BOOST_LOG(info) << "fork: camera: asking ["sv << mac_name << "] for its camera at "sv << start.width << 'x' << start.height << ", "sv << start.kbps << " kbps"sv;
              connection->send(luzcamera::make_start(start));
              decoder.close();
              decoder_failed = false;
              last_camera_frame.reset();
              gate.reset(now, false);
              break;
            }
          case camera_policy::start_stop_t::action_e::stop:
            BOOST_LOG(info) << "fork: camera: telling ["sv << mac_name << "] to turn its camera off"sv;
            connection->send(luzcamera::make_empty(luzcamera::type_e::stop));
            decoder.close();
            last_camera_frame.reset();
            break;
          case camera_policy::start_stop_t::action_e::none:
            break;
        }

        if (overflow) {
          gate.reset(now, true);
        }

        // The Mac's frames into the camera, while an app watches
        if (connection && policy.started() && apps.streaming && mf) {
          for (const auto &frame : frames) {
            bool keyframe = frame.keyframe && luzcamera::is_decodable_keyframe(frame.au.data(), frame.au.size());
            auto verdict = gate.frame(keyframe, frame.size, now);
            if (verdict == camera_policy::frame_gate_t::verdict_e::drop_and_ask) {
              connection->send(luzcamera::make_empty(luzcamera::type_e::keyframe));
              continue;
            }
            if (verdict == camera_policy::frame_gate_t::verdict_e::drop) {
              continue;
            }
            if (frame.size.width > camera_policy::k_max_width || frame.size.height > camera_policy::k_max_height || (frame.size.width & 1) || (frame.size.height & 1)) {
              continue;
            }
            if (!decoder.is_open(frame.size)) {
              if (decoder_failed || !decoder.open(frame.size)) {
                decoder_failed = true;
                continue;
              }
            }
            bool decoded = decoder.decode(frame, [&](const std::uint8_t *nv12, camera_policy::frame_size_t size) {
              auto now_us = (std::uint64_t) std::chrono::duration_cast<std::chrono::microseconds>(clock::now().time_since_epoch()).count();
              if (shared::publish(m_Frames, size.width, size.height, shared::kind_e::camera, now_us, nv12)) {
                SetEvent(m_FrameEvent);
                m_ShownPlaceholder.clear();
                last_camera_frame = now;
              }
            });
            if (!decoded) {
              decoder.close();
              gate.reset(now, true);
            }
          }
        }

        // The placeholder, while no fresh frame comes from the Mac: its frames stop for a few
        // seconds before it says why, so a network hiccup only freezes the picture
        if (apps.streaming) {
          last_size = apps.size;
          bool fresh = connection && policy.started() && last_camera_frame && now - *last_camera_frame < 3s;
          if (!fresh) {
            camera_policy::placeholder_e state;
            std::string detail;
            if (!connection) {
              auto client = camera_stream_client();
              state = client ? camera_policy::placeholder_e::connecting : camera_policy::placeholder_e::no_stream;
              if (client) {
                mac_name = *client;
              }
            } else if (status && camera_policy::placeholder_for(status->state)) {
              state = *camera_policy::placeholder_for(status->state);
              detail = status->detail;
            } else {
              state = camera_policy::placeholder_e::starting;
            }
            show_placeholder(apps.size, camera_policy::placeholder_text(state, mac_name, detail));
          }
        } else if (apps_were_streaming) {
          // The next app to open the camera starts from the placeholder, not the last frame
          show_placeholder(last_size, camera_policy::placeholder_text(connection ? camera_policy::placeholder_e::starting : camera_policy::placeholder_e::no_stream, mac_name));
        }
        apps_were_streaming = apps.streaming;
      }

      decoder.close();
      if (mf) {
        MFShutdown();
      }
      if (SUCCEEDED(co)) {
        CoUninitialize();
      }
    }

    /// Whether Windows has the virtual camera API (Windows 11)
    bool virtual_cameras_available() {
      HMODULE module = LoadLibraryExW(L"mfsensorgroup.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
      if (!module) {
        return false;
      }
      bool available = GetProcAddress(module, "MFCreateVirtualCamera") != nullptr;
      FreeLibrary(module);
      return available;
    }
  }  // namespace

  bool supported() {
    return g_supported.load();
  }

  void start() {
    if (!std::filesystem::exists(user_helper::path(L"apollo-camera.exe"))) {
      BOOST_LOG(info) << "fork: camera: this build has no Mac Camera (it's built with Visual Studio: scripts/fork/build-camera.ps1)"sv;
      return;
    }
    if (!virtual_cameras_available()) {
      if (fork_tweaks::camera_enabled) {
        BOOST_LOG(info) << "fork: camera: Mac Camera needs Windows 11's virtual cameras"sv;
      }
      return;
    }
    if (!fork_tweaks::camera_enabled) {
      // Off: the camera goes from Windows, without holding up the start
      g_registrar = std::thread([]() {
        if (run_tool(L"unregister") == 0) {
          BOOST_LOG(info) << "fork: camera: fork_camera is off: Mac Camera removed from Windows"sv;
        }
      });
      return;
    }
    auto service = std::make_unique<service_t>();
    if (!service->open()) {
      return;
    }
    g_service = std::move(service);
    g_service->run_async();
    g_supported = true;
    // Again at each start, in case Windows lost it, and for the DLL of this version
    g_registrar = std::thread([]() {
      int code = run_tool(L"register");
      if (code != 0) {
        BOOST_LOG(warning) << "fork: camera: registering Mac Camera failed ("sv << code << "); the installer's registration stays"sv;
      }
    });
  }

  void stop() {
    g_supported = false;
    if (g_registrar.joinable()) {
      g_registrar.join();
    }
    // nvhttp's thread has ended by now, so no connection calls into the service any more
    if (g_service) {
      g_service->stop();
    }
  }

  void upgrade(std::unique_ptr<nvhttp::SunshineHTTPS> socket, const request_t &request) {
    auto connection = std::make_shared<connection_t>(std::move(socket), request);
    if (request.path != luzcamera::k_path || request.upgrade != luzcamera::k_upgrade) {
      connection->refuse(404, "not_found");
      return;
    }
    if (!supported() || !g_service) {
      connection->refuse(404, "unsupported");
      return;
    }
    if (!request.verified || !request.permitted) {
      BOOST_LOG(info) << "fork: camera: ["sv << request.client_name << "] may not give this PC its camera (no Camera permission)"sv;
      connection->refuse(401, "permission");
      return;
    }
    auto session = rtsp_stream::find_session(request.client_uuid);
    if (!session || !stream::session::fork_camera_client(*session)) {
      connection->refuse(403, "not_streaming");
      return;
    }
    if (!g_service->attach(connection)) {
      BOOST_LOG(info) << "fork: camera: ["sv << request.client_name << "]'s camera waits: another client's is connected"sv;
      connection->refuse(409, "in_use");
      return;
    }
    connection->accept(session);
  }
#else
  bool supported() {
    return false;
  }

  void start() {
  }

  void stop() {
  }

  void upgrade(std::unique_ptr<nvhttp::SunshineHTTPS> socket, const request_t &) {
    boost::system::error_code ignored;
    socket->lowest_layer().close(ignored);
  }
#endif
}  // namespace fork_tweaks::camera
