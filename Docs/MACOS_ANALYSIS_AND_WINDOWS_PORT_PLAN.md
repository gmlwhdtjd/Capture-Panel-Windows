# Capture Panel macOS 분석 및 Windows Core/CLI 이식 계획

## 문서 상태

- 작성일: 2026-07-10
- 분석 대상: `Capture-Panel-Mac`
- 기준 커밋: `8499e22eb7689748c610075606606790114c1e34` (`Bump version to 0.1.1`)
- Windows 구현 저장소: `Capture-Panel-Windows`
- 범위: UI를 제외한 오디오 코어와 CLI
- 목적: macOS 구현을 그대로 번역하는 것이 아니라, 제품 동작과 검증 알고리즘을 보존하면서 CoreAudio 계층을 Windows ASIO 계층으로 교체하기 위한 기준을 만든다.

> 후속 결정: 네이티브 코어/CLI는 C++20과 CMake로, GUI는 안정된 C ABI 위의 .NET 10 WPF로 구현한다. ASIO를 제외한 전체 코어와 CLI 파이프라인은 결정론적 Fake backend로 이식했다. 이 문서의 미결정 표현과 환경 스냅샷은 당시 분석 기록이며, 현재 결정은 `WINDOWS_ARCHITECTURE.md`, 구현 상태는 `PORT_STATUS.md`를 기준으로 한다.

이 문서는 구현 전 분석 결과와 1차 설계 방향을 보존한 역사적 문서다. ASIO SDK 라이선스와 실제 대상 오디오 인터페이스의 드라이버 동작을 확인한 후 일부 세부 설계는 조정될 수 있다.

## 1. 결론 요약

Capture Panel의 핵심 가치는 CoreAudio 자체가 아니라 다음 파이프라인에 있다.

1. WAV 소스를 32-bit float 내부 포맷으로 읽는다.
2. 소스 앞에 다섯 개의 정렬 마커와 고정 길이 무음을 붙인다.
3. 선택한 출력 채널로 재생하면서 선택한 입력 채널을 동시에 녹음한다.
4. 녹음에서 마커를 찾아 장비 왕복 지연을 계산한다.
5. 원본 길이에 맞춰 녹음을 정렬하고 부족한 끝부분은 0으로 채운다.
6. 별도 검증 신호로 라우팅, 클리핑, 신호 유무, 타이밍 안정성을 판정한다.
7. 정렬된 결과를 지정 비트 깊이의 WAV로 저장한다.

macOS 구현은 이 제품 로직과 CoreAudio 코드가 이미 어느 정도 분리되어 있다. `RawAudioCapturing` 포트 아래의 정렬, 검증, 레벨 계산, 채널 문자열 파싱, 진행 이벤트는 Windows에서 개념과 테스트를 거의 그대로 옮길 수 있다. 반면 장치 ID, WAV I/O, 오류 타입 일부까지 CoreAudio 타입이 노출되어 있으므로 소스 코드를 그대로 공유하기보다는 플랫폼 독립 모델로 다시 정의하는 편이 안전하다.

Windows 1차 버전의 오디오 정책은 다음과 같이 잡는다.

- 하나의 ASIO 드라이버가 제공하는 입력과 출력을 full-duplex로 사용한다.
- 재생과 녹음은 같은 ASIO 콜백 및 같은 하드웨어 클록 도메인에서 처리한다.
- 서로 다른 두 ASIO 드라이버를 애플리케이션에서 동시에 열어 합성하지 않는다.
- 여러 물리 장치가 필요하면 제조사 멀티 디바이스 드라이버 또는 사용자가 설치한 통합 ASIO 드라이버가 하나의 드라이버로 노출하는 구성을 사용한다.
- macOS의 private aggregate device와 `--clock-source`는 Windows 1차 버전에 직접 대응시키지 않는다.

확정 구현 스택은 **C++20 + CMake + Visual Studio 2026/MSVC v145**이며, 후속 backend에서 Steinberg ASIO SDK를 연결한다. ASIO의 C++ 인터페이스 및 콜백 모델과 직접 맞고, 실시간 스레드에서 메모리와 변환 비용을 명시적으로 제어하기 쉽기 때문이다.

## 2. macOS 저장소 개요

### 2.1 코드 규모

| 영역 | Swift 파일 | 코드 줄 수 | 역할 |
|---|---:|---:|---|
| `CapturePanelCore` | 39 | 3,947 | 장치, 캡처, 정렬, 검증, WAV |
| `CapturePanel` CLI | 8 | 699 | 명령 파싱과 콘솔 출력 |
| `CapturePanelApp` | 25 | 2,781 | macOS SwiftUI 앱 |
| Core의 `CoreAudio` 폴더 | 11 | 1,379 | HAL AudioUnit 및 장치/aggregate 처리 |
| Core의 `Domain` 폴더 | 14 | 1,679 | 캡처 오케스트레이션, 정렬, 검증 |
| Core의 `Interface` 폴더 | 13 | 734 | 공개 모델, WAV, 오류, 채널 API |

Core/CLI 테스트에는 `func test...` 기준 86개 테스트가 있다. 이 중 Core 테스트가 78개, CLI 테스트가 8개다. UI 테스트는 Windows 1차 범위에서 제외한다.

### 2.2 빌드 대상

macOS 프로젝트는 Tuist와 mise로 관리되며 다음 주요 타깃을 가진다.

```text
CapturePanelCore        static framework, macOS 13+
        ↑
CapturePanel            command-line tool
        ↑
CapturePanelApp         SwiftUI/AppKit app
```

외부 Swift 패키지는 `swift-argument-parser` 하나다. Core와 CLI는 `CoreAudio` 및 `AudioToolbox` 프레임워크에 연결된다. Swift Package Manager는 실제 빌드 시스템이 아니라 Tuist 외부 의존성 선언에만 사용된다.

### 2.3 소스 구조

```text
Sources/
├─ CapturePanelCore/
│  └─ Source/
│     ├─ CoreAudio/                 macOS 오디오 백엔드
│     ├─ Domain/
│     │  ├─ Audio/                 float 변환, peak/RMS/dBFS
│     │  ├─ Capture/               유스케이스와 실행기
│     │  │  ├─ Alignment/          마커 검출 및 payload 정렬
│     │  │  └─ Verification/       sweep 생성, 상관 분석, 판정
│     │  └─ Ports/                 RawAudioCapturing 경계
│     ├─ Interface/
│     │  ├─ AudioFile/             WAV 읽기/쓰기와 포맷
│     │  ├─ Capture/               공개 설정, 결과, 이벤트
│     │  ├─ Channels/              채널 파싱과 검증
│     │  ├─ Devices/               장치/채널 모델
│     │  └─ Errors/                통합 오류
│     └─ Support/                  CoreAudio 포맷/통계 확장
├─ CapturePanel/                    CLI
└─ CapturePanelApp/                 UI — Windows 1차 범위 제외
```

## 3. 현재 런타임 아키텍처

### 3.1 상위 호출 흐름

```text
CLI command
  → CapturePassService
    → CapturePassUseCase
      → CaptureConfigurationValidator
      → CapturePassExecutor
        → RawAudioCapturing
          → AudioEngine (CoreAudio HAL)
        → CapturePayloadAligner
        → CaptureAlignmentVerificationEvaluator
      → WAVCaptureOutputWriter
```

`CapturePassService`는 라이브 의존성을 조립하는 facade다. 파일과 장치 검증, 샘플레이트 변경, raw capture, 정렬, 검증, WAV 출력이 `CapturePassUseCase`에서 순서대로 실행된다. CLI는 이벤트를 콘솔 메시지로 변환할 뿐 핵심 알고리즘을 직접 수행하지 않는다.

### 3.2 핵심 포트

플랫폼 교체 경계는 이미 `RawAudioCapturing`으로 존재한다.

```text
RawAudioCaptureRequest
  route
  playbackData             float32 interleaved
  playbackChannelCount
  totalFrames
  sampleRate
  paddingSeconds
  cancellationToken
  progressHandler

RawAudioCaptureResult
  recordedData             float32 interleaved
  prePadFrames
  recordChannelCount
```

Windows에서는 이 계약을 C++ 타입으로 옮기고 `AsioCaptureBackend`가 구현하도록 한다. 단, 현재 `route`가 `AudioDeviceID`를 포함하므로 장치 식별자를 먼저 플랫폼 독립 문자열 ID로 바꿔야 한다.

## 4. 캡처 파이프라인 상세

### 4.1 입력 검증

`CaptureConfigurationValidator`의 처리 순서는 다음과 같다.

1. 입력 경로 존재 여부 확인
2. WAV 읽기
3. 재생/녹음 장치 해석
4. 1-based 재생 및 녹음 채널 범위 검증
5. 소스 채널 수와 선택 재생 채널 수가 다르면 경고
6. 소스 peak가 `0.999` 이상이면 near-full-scale 경고

설정 검증은 의존성 주입이 되어 있어 테스트하기 좋다. 이 구조는 Windows에서도 유지한다.

### 4.2 내부 오디오 포맷

WAV는 읽는 즉시 다음 내부 표현으로 변환된다.

- 샘플: normalized `float32`
- 프레임 배치: interleaved
- 채널 인덱스: 사용자 입력은 1-based, 내부 백엔드는 0-based
- 지원 출력 비트 깊이: PCM 16/24/32
- 샘플레이트: 소스 WAV 샘플레이트를 캡처 요청 샘플레이트로 사용

CoreAudio 콜백은 장치 쪽에서 non-interleaved float32 버퍼를 사용한다. 재생 콜백은 interleaved 소스에서 선택 채널을 꺼내 각 장치 출력 버퍼에 쓰고, 입력 콜백은 선택 장치 채널을 interleaved 녹음 버퍼에 모은다.

ASIO 드라이버는 채널별 double buffer와 여러 정수/부동소수 샘플 타입을 제공할 수 있으므로 Windows 백엔드는 **ASIO native sample ↔ normalized float32** 변환 계층이 반드시 필요하다.

### 4.3 정렬용 재생 계획

`CapturePassExecutor.makeAlignmentPlaybackPlan`은 원본 앞에 정렬 preamble을 만든다.

| 값 | 현재 정책 |
|---|---:|
| 마커 수 | 5 |
| 마커 간격 | 0.1초 |
| 마커 레벨 | -12 dBFS |
| 마지막 마커 뒤 payload까지 무음 | 5.0초 |
| 녹음 전/후 padding | 각각 0.5초 |

모든 재생 채널에 같은 마커가 들어가며, playback gain은 마커와 payload 양쪽에 적용된다. 실제 녹음 길이는 다음과 같다.

```text
pre-pad 0.5s
+ marker preamble
+ marker-to-payload silence 5.0s
+ source payload
+ post-pad 0.5s
```

녹음은 오디오 엔진 시작과 함께 즉시 시작하고, 재생은 pre-pad만큼 지연시킨다.

### 4.4 마커 기반 정렬

`CapturePayloadAligner`는 예상 마커 위치 주변만 검색해 payload의 시작점을 결정한다.

1. 선택 녹음 채널 전체의 프레임 peak로 후보 onset을 찾는다.
2. 검색 구간 peak의 50%와 절대값 `0.005` 중 큰 값을 adaptive threshold로 사용한다.
3. 후보 중 예상 0.1초 간격에 가장 잘 맞는 마커 시퀀스를 고른다.
4. 최소 두 개 및 예상 마커의 45% 이상이 검출되어야 한다.
5. 검출 마커별 `detected - expected` 지연의 median을 왕복 지연으로 사용한다.
6. `sourceStartFrame + latency`부터 원본 프레임 수만큼 잘라낸다.
7. 녹음 끝이 부족하면 0으로 채워 원본과 정확히 같은 길이를 만든다.

다음 조건은 경고가 된다.

- 검출 마커 비율이 75% 미만
- 마커 지연 표준편차가 10 frame 초과
- 마커 간격 오차가 1,000 ppm 초과

마커 증거가 부족하면 임의의 payload transient를 시작점으로 사용하지 않고 캡처를 실패시킨다. 이 실패 우선 정책은 Windows에서도 보존해야 한다.

### 4.5 setup verification

`test` 명령은 실제 캡처와 같은 경로를 사용하되 원본 WAV 대신 알려진 검증 신호를 생성한다.

검증 신호 구성:

- 시작 무음 0.1초
- logarithmic sweep 0.3초
- 끝 무음 0.5초
- 주파수 80 Hz부터 `min(18 kHz, sampleRate × 0.45)`까지
- 0.01초 fade
- 기본 레벨 -12 dBFS

정렬 후 sweep 상관 분석과 레벨 분석으로 다음을 평가한다.

- 디지털 클리핑: peak가 -0.1 dBFS 이상
- 신호 없음: peak가 `0.001` 미만
- sweep 타이밍 오차: 최소 8 frame 또는 0.01초 허용 범위
- direct match와 별도 강한 echo의 모호성
- 앞부분 장비 decay가 측정에 영향을 줄 가능성

결과는 warnings와 failure reasons로 나뉜다. failure가 없을 때만 `passed == true`다.

### 4.6 샘플레이트 복원

캡처 전 소스 WAV 샘플레이트를 선택 장치에 설정하고, 성공·실패 어느 경로에서도 원래 샘플레이트를 복원한다. 두 장치일 때는 설정의 역순으로 복원한다. 이 동작은 유스케이스 단위 테스트로 보호되어 있다.

Windows에서는 ASIO 드라이버의 `canSampleRate`, `setSampleRate`, `getSampleRate`를 이용하되 다음 차이를 반영해야 한다.

- 같은 ASIO 드라이버 안의 입력/출력은 하나의 샘플레이트를 공유한다.
- 드라이버가 실행 중 샘플레이트 변경을 거부할 수 있다.
- 다른 애플리케이션이 드라이버를 점유하면 초기화 또는 변경이 실패할 수 있다.
- 종료 시 원래 샘플레이트 복원을 시도하되 드라이버 오류를 명확히 보고해야 한다.

## 5. macOS CoreAudio 백엔드

### 5.1 장치 열거

`AudioDeviceManager`는 CoreAudio system object에서 모든 `AudioDeviceID`를 읽고 다음 정보를 만든다.

- 숫자 ID
- 표시 이름
- 안정 UID
- 입력/출력 채널 수
- nominal sample rate
- 채널별 이름

Capture Panel이 만든 private aggregate device는 이름 접두사 `CapturePanel_`로 숨긴다.

### 5.2 같은 장치

재생과 녹음 장치가 같으면 하나의 HAL output AudioUnit을 full-duplex로 설정한다.

- input과 output I/O 활성화
- 선택 장치를 AudioUnit에 연결
- 장치 양방향 채널 수 조회
- 양쪽 스트림을 float32 non-interleaved로 설정
- render callback과 input callback 설치
- AudioUnit 초기화 후 시작

하나의 장치 클록을 사용하므로 sample-accurate 동기화가 가능하다.

### 5.3 서로 다른 장치

재생과 녹음 장치가 다르면 임시 private aggregate device를 만든 뒤, 그 aggregate device 하나를 같은 full-duplex AudioUnit 경로에 연결한다.

- 선택 clock source 장치: drift compensation 비활성
- 반대 장치: high-quality drift compensation 활성
- aggregate가 보고한 sub-device 순서와 채널 수를 검증
- 원래 장치의 사용자 채널을 aggregate 전체 채널 인덱스로 변환
- 캡처 종료 및 모든 오류 경로에서 aggregate device 삭제

CoreAudio가 제공하는 이 aggregate/drift compensation 기능은 Windows ASIO에 일대일 대응하지 않는다.

### 5.4 실시간 콜백

렌더 콜백은 먼저 모든 출력 버퍼를 0으로 지운 뒤 pre-pad가 끝났을 때 선택 채널에 재생 데이터를 복사한다. 입력 콜백은 `AudioUnitRender`로 전체 입력을 받은 다음 선택 채널만 미리 할당한 녹음 버퍼로 복사한다.

큰 버퍼 할당, 파일 I/O, 정렬, 상관 분석은 실시간 콜백 밖에서 수행된다. Windows ASIO 콜백도 같은 규칙을 따라야 한다.

## 6. 재사용 범위와 재작성 범위

| 기능 | Windows 판단 | 비고 |
|---|---|---|
| 채널 문자열 파싱 (`1,2`, `1-4`) | 알고리즘 이식 | 테스트 그대로 포팅 가능 |
| 채널 범위 검증 | 알고리즘 이식 | 장치 모델만 교체 |
| float 데이터 변환/추출 | 알고리즘 이식 | `Data` 대신 `std::vector<float>`/`std::span` |
| peak/RMS/dBFS | 알고리즘 이식 | 플랫폼 독립 |
| 마커 재생 계획 | 알고리즘 이식 | 상수와 테스트 보존 |
| 마커 후보/시퀀스 매칭 | 알고리즘 이식 | 플랫폼 독립 |
| payload 정렬/zero padding | 알고리즘 이식 | 플랫폼 독립 |
| verification signal/sweep | 알고리즘 이식 | 플랫폼 독립 |
| sweep correlation/evaluator | 알고리즘 이식 | 플랫폼 독립 |
| 캡처 유스케이스/이벤트 | 구조 이식 | C++ 의존성 주입으로 재구성 |
| WAV 읽기/쓰기 | 재작성 | AudioToolbox 제거 |
| 장치 열거/채널 정보 | 재작성 | CoreAudio → ASIO driver registry/API |
| 실시간 오디오 엔진 | 재작성 | HAL AudioUnit → ASIO callbacks |
| sample rate 설정 | 재작성 | CoreAudio property → ASIO API |
| private aggregate device | 제외 | Windows 1차 정책에 없음 |
| aggregate 채널 매핑 | 제외 | 단일 ASIO 드라이버 채널 인덱스 사용 |
| OSStatus 오류 해석 | 재작성 | `ASIOError`, Win32/COM 오류 사용 |
| CLI 명령과 메시지 | 계약 이식 | 장치 옵션은 Windows 모델에 맞게 변경 |
| SwiftUI/AppKit 앱 | 제외 | 현재 작업 범위 아님 |

### 6.1 제거해야 할 플랫폼 타입 누수

현재 다음 플랫폼 타입이 Domain/Interface에 노출된다.

- `AudioDeviceID`
- `OSStatus`
- `AudioStreamBasicDescription`
- `ExtAudioFileRef`

Windows 모델에서는 다음처럼 바꾼다.

```text
DeviceId       = stable string/UUID (ASIO driver name + CLSID 기반)
ChannelIndex   = uint32, 공개 CLI는 1-based
SampleRate     = double
AudioBuffer    = interleaved normalized float32
BackendError   = 자체 error code + 원본 ASIO/Win32 code + context
```

## 7. Windows ASIO 설계 방향

### 7.1 ASIO 장치 모델

CoreAudio처럼 입력 장치와 출력 장치를 독립 선택하지 않고, ASIO 드라이버 하나를 선택한다.

```text
AsioDriverInfo
  id                 안정 식별자
  name               표시 이름
  version
  inputChannels
  outputChannels
  currentSampleRate
  supportedSampleRates (탐색 가능한 범위)
  min/max/preferred/granularity buffer size

AsioChannelInfo
  direction          input | output
  index              내부 0-based
  displayIndex       CLI 1-based
  name
  sampleType
  active
```

드라이버 열거는 설치된 ASIO 드라이버 등록 정보를 사용하고, 실제 채널/샘플레이트/버퍼 정보는 드라이버를 초기화한 뒤 질의한다. 일부 드라이버는 동시에 하나만 열리거나 다른 앱과 독점 충돌할 수 있으므로 `devices` 명령도 개별 드라이버 질의 실패를 전체 실패로 만들지 않고 상태를 표시하는 방식을 고려한다.

### 7.2 1차 지원 토폴로지

```text
source WAV
   ↓
CapturePassExecutor
   ↓
AsioCaptureBackend
   ↓
one ASIO driver / one callback clock
   ├─ selected output channels → external hardware
   └─ selected input channels  ← external hardware
```

이 정책의 장점은 macOS same-device 경로와 의미가 같고, 입출력 버퍼가 같은 `bufferSwitch` 주기와 sample position을 공유한다는 점이다.

1차 버전에서 지원하지 않는 것:

- 애플리케이션이 서로 다른 ASIO 드라이버 두 개를 동시에 열기
- 소프트웨어 resampling으로 두 하드웨어 클록을 추적하기
- CoreAudio식 임시 aggregate device 생성
- WDM/WASAPI 장치와 ASIO 장치 혼합

이 기능들은 단순 추가가 아니라 별도의 clock drift estimator, ring buffer, adaptive resampler, underrun/overrun 정책을 요구하므로 후속 설계로 분리한다.

### 7.3 ASIO 콜백 처리

ASIO 콜백은 다음 일만 해야 한다.

1. 현재 double-buffer index의 모든 선택/비선택 출력 채널을 silence로 초기화
2. pre-pad 이후 선택 출력 채널에 playback float 데이터를 native ASIO sample로 변환하여 기록
3. 선택 입력 채널의 native ASIO sample을 normalized float로 변환하여 preallocated record buffer에 기록
4. 원자적 frame counter와 완료/오류 플래그 갱신
5. 드라이버가 요구하면 `outputReady` 호출

콜백에서 금지할 작업:

- heap allocation/deallocation
- mutex 대기
- 파일 I/O 또는 콘솔 출력
- 예외 전파
- 장치 열거/제어판 호출
- 마커 검색, FFT/상관 분석, WAV 인코딩

제어 스레드는 완료 신호, 취소, timeout, `sampleRateDidChange`, reset request를 처리한다. 콜백 수명 중 접근하는 모든 버퍼와 상태는 `start` 전에 할당하고 `stop` 및 callback 종료가 보장된 뒤 해제한다.

### 7.4 샘플 타입 변환

ASIO 채널은 드라이버마다 sample type이 다를 수 있다. 최소한 실제 대상 장비에서 확인된 타입을 먼저 지원하되 설계는 다음 계열을 수용해야 한다.

- 16/24/32-bit signed integer
- 32/64-bit float
- little-endian 타입
- 드라이버가 보고할 수 있는 기타 정렬/endianness 타입은 명시적으로 unsupported 처리

변환기는 채널별 함수 포인터 또는 전략을 `createBuffers` 직후 선택해 콜백 내부 분기를 줄인다. 모든 도메인 알고리즘은 normalized float32만 본다.

### 7.5 버퍼 크기와 정렬

ASIO 버퍼 크기는 드라이버의 min/max/preferred/granularity 정책을 따른다. 1차 기본은 preferred size를 사용한다. 캡처의 frame count는 ASIO block size의 배수가 아닐 수 있으므로 마지막 콜백에서 필요한 프레임만 복사하고 나머지 출력은 silence로 유지한다.

녹음 완료 조건은 macOS와 동일하게 `prePadFrames + playbackPlanFrames + postPadFrames`에 도달했을 때다. 드라이버 지연값은 진단 정보로 기록하되, 최종 payload 정렬은 실제 하드웨어 루프에서 검출한 마커 지연을 기준으로 한다.

### 7.6 드라이버 이벤트

다음 ASIO 이벤트를 오류/재시작 정책에 연결해야 한다.

- sample rate changed
- reset request
- resync request
- latency changed
- engine version/time info support

캡처 도중 reset, resync 또는 예상하지 않은 sample rate 변경이 발생하면 조용히 계속하지 말고 해당 pass를 실패시키는 것이 1차 정책이다.

## 8. 권장 Windows 프로젝트 구조

```text
Capture-Panel-Windows/
├─ CMakeLists.txt
├─ CMakePresets.json
├─ README.md
├─ LICENSE                     배포 정책 결정 후 확정
├─ Docs/
│  └─ MACOS_ANALYSIS_AND_WINDOWS_PORT_PLAN.md
├─ cmake/
├─ external/
│  └─ asio/                    SDK 원문 라이선스/고지 확인 후 추가
├─ src/
│  ├─ core/
│  │  ├─ audio/                AudioBuffer, level, conversion
│  │  ├─ capture/              config, use case, executor, events
│  │  ├─ alignment/            marker detection and alignment
│  │  ├─ verification/         signal, sweep matcher, evaluator
│  │  ├─ devices/              platform-neutral device/channel models
│  │  ├─ wav/                  WAV abstraction
│  │  └─ errors/
│  ├─ platform/
│  │  └─ asio/
│  │     ├─ AsioDriverRegistry
│  │     ├─ AsioDriverSession
│  │     ├─ AsioCaptureBackend
│  │     ├─ AsioSampleConverter
│  │     └─ AsioCallbacks
│  └─ cli/
│     ├─ main.cpp
│     ├─ DevicesCommand
│     ├─ ChannelsCommand
│     ├─ TestCommand
│     └─ RunCommand
└─ tests/
   ├─ core/
   ├─ cli/
   ├─ platform/
   └─ fixtures/
```

핵심 의존성 방향은 다음을 강제한다.

```text
cli → core ← platform/asio
```

`core`는 Windows 헤더와 ASIO 헤더를 include하지 않는다. ASIO 백엔드는 core가 정의한 `IAudioCaptureBackend` 및 `IAudioDeviceProvider`를 구현한다.

## 9. 권장 핵심 인터페이스

```cpp
struct RawAudioCaptureRequest {
    CaptureRoute route;
    std::span<const float> playbackInterleaved;
    std::uint32_t playbackChannels;
    std::int64_t playbackFrames;
    double sampleRate;
    double paddingSeconds;
    CancellationToken* cancellation;
    ProgressCallback progress;
};

struct RawAudioCaptureResult {
    std::vector<float> recordedInterleaved;
    std::int64_t prePadFrames;
    std::uint32_t recordChannels;
};

class IAudioCaptureBackend {
public:
    virtual ~IAudioCaptureBackend() = default;
    virtual RawAudioCaptureResult capture(
        const RawAudioCaptureRequest& request) = 0;
};
```

실제 API에서는 오류 전달 방식(`std::expected`, 자체 `Result`, 예외)을 프로젝트 전체에서 하나로 통일한다. 실시간 콜백을 넘어 예외가 전파되어서는 안 된다.

## 10. CLI 이식안

기존 명령 이름은 유지한다.

```text
capture-panel devices
capture-panel channels
capture-panel test
capture-panel run
```

Windows 1차 명령 계약 제안:

```powershell
capture-panel devices

capture-panel channels `
  --driver <driver-id>

capture-panel test `
  --driver <driver-id> `
  --play-channel 1 `
  --record-channel 1 `
  --output-trim 0 `
  --verbose

capture-panel run `
  --input source.wav `
  --output recorded.wav `
  --driver <driver-id> `
  --play-channel 1 `
  --record-channel 1 `
  --bit-depth 24
```

macOS와 다른 점:

- `--play-device`와 `--record-device` 대신 `--driver` 하나를 사용한다.
- `--clock-source`는 제거한다. 한 ASIO 드라이버의 클록을 사용하기 때문이다.
- 채널 스펙과 `--bit-depth`의 문법은 유지한다.
- `test`/`run`의 진행 단계와 진단 메시지는 가능한 한 동일하게 유지한다.

향후 서로 다른 백엔드를 지원하게 되면 `--backend asio`를 추가할 수 있지만 ASIO만 있는 1차 버전에는 불필요하다.

## 11. 테스트 이식 전략

### 11.1 바로 이식할 테스트

- ChannelParser 및 ChannelValidator
- WAVFormat 계산
- AudioDataConverter
- AudioSignalLevel
- CapturePassOptions gain
- CapturePassExecutor playback plan
- CaptureMarkerSequenceMatcher
- CapturePayloadAligner
- verification signal/evaluator/sweep matcher
- CaptureConfigurationValidator의 의존성 주입 테스트
- sample rate 복원 순서 및 실패 경로
- CLI 옵션 파싱

### 11.2 교체할 테스트

- `AudioStreamBasicDescription` 생성 테스트 → WAV/native sample converter 테스트
- `AudioBufferList` 재사용 테스트 → ASIO double-buffer adapter 테스트
- aggregate channel mapper 테스트 → 단일 ASIO 드라이버 channel mapper 테스트
- OSStatus 설명 테스트 → ASIO/Win32 error context 테스트

### 11.3 하드웨어 통합 테스트

자동 단위 테스트만으로 보장할 수 없는 항목:

- 설치 ASIO 드라이버 열거 및 초기화
- 장치별 native sample type 변환
- 입력/출력 채널 이름과 순서
- 실제 buffer size와 reported latency
- 장시간 캡처의 drift
- 외부 케이블/패치베이 라우팅
- 다른 앱이 장치를 점유한 상태의 오류
- sample rate 변경과 복원
- reset/resync 이벤트

첫 대상 장비를 정한 뒤 장비명, 드라이버 버전, sample rate, buffer size를 고정한 수동 검증 매트릭스를 별도 문서로 만든다.

## 12. 구현 단계

### Phase 0 — 결정과 도구

- 배포 라이선스: GPL-3.0-only 공개 배포로 결정
- Visual Studio 2026의 Desktop development with C++ 설치
- CMake 4.2 이상과 Windows SDK 준비
- ASIO SDK 취득 및 저장소 포함 정책 결정
- 첫 검증 대상 ASIO 장비/드라이버 확정

상태: ASIO SDK와 첫 실기기 선정만 후속 작업이다. C++20 CLI와 테스트 프로젝트는 생성됐다.

### Phase 1 — 플랫폼 독립 코어

- 공통 오디오/장치/설정/결과 모델
- 채널 parser/validator
- WAV abstraction 및 float32 내부 버퍼
- level 계산과 gain
- alignment playback plan
- marker matcher/aligner
- verification signal/evaluator
- macOS Core 테스트의 플랫폼 독립 부분 포팅

상태: Fake backend, 단위 테스트, end-to-end 코어/CLI 테스트까지 구현 완료. 실제 빌드 결과는 `PORT_STATUS.md`에 기록한다.

### Phase 2 — ASIO 탐색 계층

- 드라이버 registry 및 session RAII
- `devices`, `channels` 명령
- channel/sample type/sample rate/buffer size 질의
- 오류와 점유 상태 진단

완료 조건: 대상 장비를 열거하고 모든 입출력 채널 정보를 CLI로 출력한다.

### Phase 3 — raw full-duplex capture

- ASIO double buffers와 callbacks
- native sample converter
- preallocated playback/record buffers
- pre/post padding, 선택 채널 mapping
- cancellation, timeout, reset/resync 처리
- loopback raw capture smoke test

완료 조건: 대상 장비의 한 출력과 한 입력을 통해 알려진 신호를 손실 없이 왕복 녹음한다.

### Phase 4 — 제품 파이프라인 연결

- `CapturePassUseCase`와 ASIO backend 연결
- marker alignment
- setup verification
- aligned WAV writer
- `test`, `run` CLI와 진단 출력

완료 조건: macOS와 같은 source → hardware loop → aligned WAV 시나리오가 통과한다.

### Phase 5 — 안정화

- 다양한 buffer size/sample type/sample rate
- 다채널 route
- 장치 점유/분리/드라이버 reset
- 긴 캡처와 메모리 상한
- 패키징 및 코드 서명

## 13. 주요 위험과 결정 사항

### 13.1 ASIO SDK 라이선스

2026-07-10 확인 기준 Steinberg의 공개 ASIO SDK 경로는 GPLv3로 안내된다. 공개 변형으로 제품을 배포하면 제품의 GPLv3 호환성과 소스 공개 의무를 검토해야 한다. ASIO 명칭과 로고는 별도 상표 규칙을 따른다. 향후 비공개/상용 배포 가능성이 있다면 코드 의존성을 추가하기 전에 Steinberg의 당시 proprietary 경로를 확인해야 한다.

이 문서는 법률 자문이 아니다. 실제 배포 전 라이선스 검토가 필요하다.

공식 참고:

- [Steinberg: ASIO Open Source license variant](https://www.steinberg.net/developers/asiosdk-open/)
- [Steinberg Developer Portal](https://www.steinberg.net/developers/)

### 13.2 다중 장치

macOS 기능과 완전히 같은 “서로 다른 장치 선택”을 Windows 첫 버전에서 약속하면 일정과 안정성 위험이 매우 커진다. 단일 ASIO 드라이버 full-duplex를 명시적 제품 제한으로 두고, 실제 사용자 요구가 확인된 뒤 adaptive resampling 기반 다중 장치를 별도 기능으로 설계한다.

### 13.3 WAV 라이브러리

Windows 코어에는 외부 의존성 없는 작은 RIFF/WAVE codec을 구현했다. PCM 16/24/32와 IEEE float32 입력, `WAVE_FORMAT_EXTENSIBLE`, PCM 16/24/32 출력을 지원한다. RIFF 4 GiB 한계를 넘는 RF64는 현재 지원하지 않으며 명시적으로 거부한다.

### 13.4 전체 파일 메모리 적재

macOS 구현은 소스와 전체 녹음을 메모리에 적재한다. 구현은 단순하지만 긴 다채널 캡처에서 메모리 사용량이 커진다.

```text
bytes ≈ frames × channels × 4
```

1차 Windows 버전은 동작 일치를 위해 같은 정책을 사용할 수 있다. 이후 길이 제한, 파일-backed buffer 또는 chunked writer를 별도 최적화한다.

### 13.5 알고리즘 동등성

Swift에서 C++로 옮길 때 부동소수 계산 순서와 반올림 차이 때문에 경계 테스트가 달라질 수 있다. macOS 테스트 fixture와 기대 결과를 가능한 한 그대로 가져오고, 허용 오차를 이유 없이 넓히지 않는다.

## 14. 최초 분석 당시 Windows 개발 장치 상태

2026-07-10 점검 결과:

- Git 및 OpenSSH: 설치됨
- Windows 저장소와 작성자 설정: 준비됨
- Visual Studio / MSVC (`cl`): 없음
- CMake: 없음
- Ninja: 없음
- MSBuild: 없음
- .NET runtime은 있으나 SDK 없음
- Rust toolchain 없음
- Swift toolchain 없음

위 목록은 구현 전 스냅샷이다. 현재 설치 및 검증 상태는 `PORT_STATUS.md`와 `DEVELOPMENT.md`를 따른다. UI를 제외한 현재 범위에는 Node.js, Python 또는 Swift 설치가 필수는 아니다.

## 15. 첫 구현 전에 확정할 체크리스트

- [x] Capture Panel Windows의 배포 라이선스: GPL-3.0-only
- [ ] ASIO SDK 취득/보관/CI 주입 방식
- [ ] 첫 대상 오디오 인터페이스와 ASIO 드라이버 버전
- [x] 1차 CPU 아키텍처: x64
- [x] WAV 입력 지원 범위: PCM 16/24/32, float32, WAVE_FORMAT_EXTENSIBLE
- [ ] 최대 캡처 길이 또는 메모리 제한
- [x] CLI의 `--driver` ID: backend가 제공하는 안정 문자열
- [x] C++ 오류 전달 방식과 테스트 프레임워크: `CaptureError` + CTest 경량 harness
- [x] 외부 의존성 관리 방식: 코어는 무의존 CMake, ASIO SDK 정책은 후속 확정

## 16. 기준 원칙

1. 도메인 코어에는 ASIO/Windows 타입을 노출하지 않는다.
2. 모든 오디오 도메인 알고리즘은 normalized interleaved float32를 사용한다.
3. 실시간 콜백에서는 할당, 잠금 대기, I/O, 로깅, 분석을 하지 않는다.
4. 라우팅 또는 타이밍을 확신할 수 없으면 조용히 잘못된 WAV를 만들지 않고 실패한다.
5. 실제 hardware-loop marker를 최종 정렬 기준으로 사용한다.
6. macOS의 검증 상수와 테스트를 변경 사유 없이 바꾸지 않는다.
7. 단일 ASIO 드라이버 full-duplex를 Windows 1차 지원 경계로 삼는다.
8. 드라이버별 차이는 core가 아니라 ASIO backend와 진단 계층에서 흡수한다.
