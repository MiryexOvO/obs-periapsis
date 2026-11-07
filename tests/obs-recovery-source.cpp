// Real libobs/NVENC test source with a 30-second periodic GOP. / 30 秒周期 GOP 的真实编码测试源。
#include <windows.h>
#include <obs-module.h>
extern "C" uint64_t os_gettime_ns();
#include <cmath>
#include <vector>
#include <thread>
#include <chrono>
#include <cstdio>
#include <string>
static void logfn(int level,const char* fmt,va_list ap,void*){if(level<=LOG_INFO){vfprintf(stderr,fmt,ap);fputc('\n',stderr);}}
static obs_module_t* load(const std::string& binary,const std::string& data){
    fprintf(stderr,"LOAD %s\n",binary.c_str());obs_module_t* m=nullptr;if(obs_open_module(&m,binary.c_str(),data.c_str()) || !obs_init_module(m)){fprintf(stderr,"MODULE FAILED %s\n",binary.c_str());exit(3);}return m;
}
int main(){
    // Network baselines use the same source with configurable duration/GOP and texture.
    // 弱网基线复用相同测试源，可配置时长、GOP 和动态纹理；恢复测试默认值保持不变。
    const bool network=getenv("PERI_OBS_TEST_NETWORK")!=nullptr;
    const int seconds=getenv("PERI_OBS_TEST_SECONDS")?atoi(getenv("PERI_OBS_TEST_SECONDS")):10;
    if(seconds<5 || seconds>180)return 11;
    const int bitrate=getenv("PERI_OBS_TEST_BITRATE")?atoi(getenv("PERI_OBS_TEST_BITRATE")):(network?20000:14000), keyint=network?1:30;
    if(bitrate<1000 || bitrate>100000)return 12;
    const bool steps=getenv("PERI_OBS_TEST_RATE_STEPS")!=nullptr;
    const bool static_texture=getenv("PERI_OBS_TEST_STATIC_TEXTURE")!=nullptr;
    const bool pan_texture=getenv("PERI_OBS_TEST_PAN_TEXTURE")!=nullptr;
    if(static_texture && pan_texture)return 13;
    base_set_log_handler(logfn,nullptr);
    if(!obs_startup("en-US",getenv("PERI_OBS_TEST_CONFIG"),nullptr))return 2;
    obs_add_data_path("D:/OBS/obs-studio/data/libobs/");
    obs_video_info v{};v.graphics_module="libobs-d3d11";v.fps_num=60;v.fps_den=1;v.base_width=v.output_width=2560;v.base_height=v.output_height=1440;v.output_format=VIDEO_FORMAT_NV12;v.gpu_conversion=true;v.colorspace=VIDEO_CS_709;v.range=VIDEO_RANGE_PARTIAL;
    if(obs_reset_video(&v)!=OBS_VIDEO_SUCCESS)return 4;
    obs_audio_info a{48000,SPEAKERS_STEREO};if(!obs_reset_audio(&a))return 5;
    for(auto n:{"obs-ffmpeg","obs-nvenc","rtmp-services"}) load(std::string("D:/OBS/obs-studio/obs-plugins/64bit/")+n+".dll",std::string("D:/OBS/obs-studio/data/obs-plugins/")+n);
    auto* m=load("D:/DSHWW/periapsis/target/obs-package/peri-output/bin/64bit/peri-output.dll","D:/DSHWW/obs-periapsis/native");
    obs_post_load_modules();fprintf(stderr,"POSTLOAD OK\n");
    obs_source_info src{};src.id="test_peri_av";src.type=OBS_SOURCE_TYPE_INPUT;src.output_flags=OBS_SOURCE_ASYNC_VIDEO|OBS_SOURCE_AUDIO;
    src.get_name=[](void*){return "test";};src.create=[](obs_data_t*,obs_source_t* s)->void*{return s;};src.destroy=[](void*){};obs_register_source(&src);
    auto* source=obs_source_create("test_peri_av","test",nullptr,nullptr);obs_set_output_source(0,source);
    fprintf(stderr,"SOURCE OK\n");bool found=false;
    for(int attempt=0;!found && attempt<30;++attempt){
      auto *props=obs_get_service_properties("rtmp_common");auto *list=obs_properties_get(props,"service");
      for(size_t i=0;i<obs_property_list_item_count(list);++i){const char *item=obs_property_list_item_string(list,i);if(item && std::string(item)=="PERI")found=true;}
      obs_properties_destroy(props);if(!found)Sleep(100);
    }
    if(!found){fprintf(stderr,"service picker missing PERI\n");return 6;}
    fprintf(stderr,"PICKER OK\n");
    auto *settings=obs_data_create();obs_data_set_string(settings,"service","PERI");
    auto *props=obs_get_service_properties("rtmp_common");obs_property_modified(obs_properties_get(props,"service"),settings);obs_properties_destroy(props);
    fprintf(stderr,"UI protocol=%s\n",obs_data_get_string(settings,"protocol"));obs_data_set_string(settings,"server",getenv("PERI_OBS_TEST_SERVER")?getenv("PERI_OBS_TEST_SERVER"):"peri://invitation");obs_data_set_string(settings,"key",getenv("PERI_OBS_TEST_TICKET"));
    auto *service=obs_service_create("rtmp_common","PERI catalog test",settings,nullptr);obs_data_release(settings);
    if(!service || std::string(obs_service_get_protocol(service))!="PERI"){fprintf(stderr,"wrong protocol\n");return 9;}
    fprintf(stderr,"Native picker contains PERI, protocol selected\n");
    auto* output=obs_output_create("peri_native_output","test",nullptr,nullptr);if(!output||!service)return 7;obs_output_set_service(output,service);
    auto *profile_settings=obs_data_create();
    obs_data_set_int(profile_settings,"bitrate",bitrate);obs_data_set_string(profile_settings,"rate_control","CBR");obs_data_set_int(profile_settings,"bf",0);obs_data_set_int(profile_settings,"keyint_sec",keyint);
    auto *profile=obs_video_encoder_create("obs_nvenc_av1_tex","test OBS saved profile",profile_settings,nullptr);
    obs_data_release(profile_settings);obs_encoder_set_video(profile,obs_get_video());obs_output_set_video_encoder(output,profile);
    const uint32_t divisor=getenv("PERI_OBS_TEST_FRAME_DIVISOR")?atoi(getenv("PERI_OBS_TEST_FRAME_DIVISOR")):1;
    if(divisor<1 || divisor>4 || !obs_encoder_set_frame_rate_divisor(profile,divisor))return 11;
    if(!obs_output_start(output)){fprintf(stderr,"START FAILED %s\n",obs_output_get_last_error(output));return 8;}
    // Recovery owns its encoder; the saved/recording profile stays untouched.
    // 恢复使用独立编码器，保留档案和录制编码器的设置。
    auto *original_settings=obs_encoder_get_settings(profile);
    const bool isolated=obs_output_get_video_encoder(output)!=profile &&
        obs_encoder_get_frame_rate_divisor(profile)==divisor &&
        obs_data_get_int(original_settings,"keyint_sec")==keyint &&
        obs_data_get_int(original_settings,"bitrate")==bitrate;
    obs_data_release(original_settings);obs_encoder_release(profile);
    if(!isolated){fprintf(stderr,"encoder isolation failed\n");return 10;}
    std::vector<uint8_t> pix(320*180*4,255);std::vector<float> pcm(800*2);
    std::vector<uint32_t> expanded(network?2560*1440:0);
    auto start=os_gettime_ns();
    for(int i=0;i<seconds*60 && obs_output_active(output);i++){
        // Isolated encoder step response; never enable the overload controller together.
        // 隔离编码器阶跃响应，测试时不同时启用拥塞控制器。
        if(steps && (i==8*60 || i==16*60)) {
            const int target=i==8*60?10000:4000;
            auto *encoder=obs_output_get_video_encoder(output);
            auto *change=obs_data_create();obs_data_set_int(change,"bitrate",target);
            obs_encoder_update(encoder,change);obs_data_release(change);
            auto *actual=obs_encoder_get_settings(encoder);
            fprintf(stderr,"RATE_STEP seconds=%d requested=%d settings=%lld encoder=%s\n",i/60,target,
                (long long)obs_data_get_int(actual,"bitrate"),obs_encoder_get_id(encoder));
            obs_data_release(actual);
        }
        if(i%6==0 && getenv("PERI_OBS_TEST_STOP") &&
            GetFileAttributesA(getenv("PERI_OBS_TEST_STOP"))!=INVALID_FILE_ATTRIBUTES)break;
        uint64_t ts=start+(uint64_t)i*1000000000/60;
        auto stamp=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        for(size_t k=0;k<pix.size();k+=4){int cell=int((k/4)%320)/8;bool white=cell<8?(cell%2==0):cell<24?((stamp>>(23-cell))&1):cell<40? !((stamp>>(39-cell))&1):false;pix[k]=pix[k+1]=pix[k+2]=white?240:16;}
        if(network) for(size_t k=320*16*4;k<pix.size();k+=4){
            uint32_t value=uint32_t(k/4)+(static_texture?0u:pan_texture?uint32_t(i):uint32_t(i)*0x9e3779b9u);
            value^=value>>16;value*=0x7feb352du;value^=value>>15;value*=0x846ca68bu;value^=value>>16;
            pix[k]=uint8_t(value);pix[k+1]=uint8_t(value>>8);pix[k+2]=uint8_t(value>>16);
        }
        // A directly attached async source is not automatically scaled by libobs.
        // 直接挂载的异步源不会自动铺满画布，显式扩大测试纹理，避免只测左上角小画面。
        if(network) for(size_t y=0;y<1440;y++){
            const auto *row=reinterpret_cast<const uint32_t *>(pix.data())+(y/8)*320;
            for(size_t x=0;x<2560;x++)expanded[y*2560+x]=row[x/8];
        }
        obs_source_frame frame{};frame.data[0]=network?reinterpret_cast<uint8_t *>(expanded.data()):pix.data();frame.linesize[0]=(network?2560:320)*4;frame.width=network?2560:320;frame.height=network?1440:180;frame.format=VIDEO_FORMAT_BGRA;frame.timestamp=ts;obs_source_output_video(source,&frame);
        for(size_t j=0;j<800;j++)pcm[2*j]=pcm[2*j+1]=float(.12*sin((i*800+j)*.057595865));
        obs_source_audio sound{};sound.data[0]=(uint8_t*)pcm.data();sound.frames=800;sound.samples_per_sec=48000;sound.speakers=SPEAKERS_STEREO;sound.format=AUDIO_FORMAT_FLOAT;sound.timestamp=ts;obs_source_output_audio(source,&sound);
        auto target=start+(uint64_t)(i+1)*1000000000/60; auto now=os_gettime_ns(); if(target>now)std::this_thread::sleep_for(std::chrono::nanoseconds(target-now));
    }
    fprintf(stderr,"BYTES %llu ERROR %s\n",(unsigned long long)obs_output_get_total_bytes(output),obs_output_get_last_error(output));
    obs_output_stop(output);while(obs_output_active(output))Sleep(10);
    obs_output_release(output);obs_service_release(service);obs_set_output_source(0,nullptr);obs_source_release(source);obs_shutdown();
}


