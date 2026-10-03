#define main magnesium_ui_main
#include "../src/main.cpp"
#undef main
#include <stdexcept>
#include <chrono>
#if defined(__APPLE__)
#include <mach/mach.h>
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>
#endif
StoreResult LoadSession() { return {StoreStatus::Missing,{}}; }
StoreStatus SaveSession(const SavedSession&) { return StoreStatus::Unavailable; }
StoreStatus DeleteSession() { return StoreStatus::Ok; }
static void check(bool ok) { if (!ok) throw std::runtime_error("Client memory probe failed"); }
static double FootprintMiB() {
#if defined(__APPLE__)
    task_vm_info_data_t info{}; mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) return info.phys_footprint/1048576.0;
#endif
    return 0;
}
int main(int argc, char** argv) {
    const int count = argc > 1 ? std::stoi(argv[1]) : 10000;
    const int frames = argc > 2 ? std::stoi(argv[2]) : 20;
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_WINDOW_HIGHDPI);
    InitWindow(600,480,"Magnesium memory verification");
    SetWindowMinSize(600,480);
    RefreshClientFonts(); GuiSetStyle(DEFAULT, TEXT_SIZE,14);
#if defined(__APPLE__)
    GLint depthBits = -1, stencilBits = -1;
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_DEPTH, GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE, &depthBits);
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_STENCIL, GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE, &stencilBits);
    check(glGetError() == GL_NO_ERROR && depthBits == 0 && stencilBits == 0);
#endif
    for (int i = 1; i <= count; ++i) chatMessages.Append({i,0,"alice",u8"café hello world wrap test text 12345"});
    auto start = std::chrono::steady_clock::now();
    const Rectangle bounds{20,72,560,338};
    for (int i = 0; i < frames; ++i) { BeginDrawing(); ClearBackground(RAYWHITE); DrawChatHistory(bounds); EndDrawing(); }
    const double measuredFootprint = FootprintMiB();
    auto elapsed = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    auto revision = chatLayout.revision; const auto* blocks = chatLayout.blocks.data();
    const auto* rows = chatLayout.blocks.front().rows.data();
    BeginDrawing(); DrawChatHistory(bounds); EndDrawing();
    check(chatLayout.revision == revision && chatLayout.blocks.data() == blocks && chatLayout.blocks.front().rows.data() == rows);
    std::size_t cachedRows = 0;
    for (const auto& block : chatLayout.blocks) cachedRows += block.rows.size();
    check(cachedRows <= 8192 && chatMessages.size() <= ChatHistory::MaxMessages);
    const float oldWrapWidth = chatLayout.width;
    BeginDrawing(); DrawChatHistory(Rectangle{20,72,700,338}); EndDrawing();
    check(chatLayout.width != oldWrapWidth);
    chatInputEditMode = false; SubmitChatMessage(); check(chatInputEditMode);
    // An in-flight send must neither spawn another thread nor discard the next input.
    std::promise<void> pending; sendFuture = pending.get_future();
    std::strcpy(chatInputBuffer,"keep this message"); SubmitChatMessage();
    check(std::string(chatInputBuffer) == "keep this message" && chatInputEditMode);
    pending.set_value(); sendFuture.get();
    std::string response; char chunk[1024]{};
    for (int i = 0; i < 1024; ++i) check(WriteCallback(chunk,1,1024,&response) == 1024);
    check(WriteCallback(chunk,1,1,&response) == 0 && response.size() == 1024*1024);
    response.clear(); response.shrink_to_fit();
    std::cout << "MEMORY_PROBE messages_received=" << count << " retained=" << chatMessages.size()
              << " accounted_bytes=" << chatMessages.AccountedBytes() << " cached_rows=" << cachedRows
              << " physical_mib=" << measuredFootprint << " render_frames=" << frames << " render_ms=" << elapsed << '\n';
    // Retiring old cached rows must preserve the reader's position in retained text.
    float removedHeight = 0;
    for (std::size_t i = 0; i < 10; ++i) removedHeight += chatLayout.blocks[i].height;
    chatScroll.followLatest = false; chatScroll.offset = -1000;
    for (int i = 1; i <= 10; ++i) chatMessages.Append({count+i,0,"alice","new message"});
    BeginDrawing(); DrawChatHistory(Rectangle{20,72,700,338}); EndDrawing();
    check(std::abs(chatScroll.offset - (-1000 + removedHeight)) < 1);
    // Prepending history preserves the same visible message despite added rows above it.
    ClearChatHistory();
    for (int i = 101; i <= 200; ++i) chatMessages.Append({i,0,"alice","anchor test"});
    chatScroll.followLatest = false; chatScroll.offset = -500;
    BeginDrawing(); DrawChatHistory(bounds); EndDrawing();
    const float previousOffset = chatScroll.offset;
    SyncResult olderPage;
    for (int i = 51; i <= 100; ++i) olderPage.messages.push_back({i,0,"alice","anchor test"});
    ApplyMessagePage(olderPage, FetchMode::Older);
    BeginDrawing(); DrawChatHistory(bounds); EndDrawing();
    float prependedHeight = 0;
    for (int i = 0; i < 50; ++i) prependedHeight += chatLayout.blocks[i].height;
    check(std::abs(chatScroll.offset - (previousOffset - prependedHeight)) < 1);
    ClearChatHistory(); check(chatMessages.empty() && chatLayout.blocks.empty());
    for (int i = 1; i <= 100; ++i) chatMessages.Append({i,0,"alice",std::string(2000, '\n')});
    BeginDrawing(); DrawChatHistory(bounds); EndDrawing();
    std::size_t newlineRows = 0;
    for (const auto& block : chatLayout.blocks) newlineRows += block.rows.size();
    check(newlineRows <= 8192 && chatMessages.size() == chatLayout.blocks.size());
    ClearChatHistory();
    // Every applied page remains visible after row-budget trimming, even with
    // 2,000 explicit newlines per message, in both directions.
    chatMessages.Append({101,0,"alice","tail"});
    while (chatMessages[0].id > 1) {
        const int cursor = chatMessages[0].id;
        SyncResult page;
        for (int id = std::max(1,cursor-50); id < cursor; ++id)
            page.messages.push_back({id,0,"alice",std::string(2000,'\n')});
        ApplyMessagePage(page, FetchMode::Older);
        const int appliedFirst = chatMessages[0].id;
        BeginDrawing(); DrawChatHistory(bounds); EndDrawing();
        check(chatMessages[0].id == appliedFirst && appliedFirst < cursor);
        check(chatMessages.size() == chatLayout.blocks.size());
    }
    while (browsingHistory) {
        const int cursor = chatMessages[chatMessages.size()-1].id;
        SyncResult page;
        for (int id = cursor+1; id <= std::min(101,cursor+50); ++id)
            page.messages.push_back({id,0,"alice",std::string(2000,'\n')});
        ApplyMessagePage(page, FetchMode::Newer);
        const int appliedLast = chatMessages[chatMessages.size()-1].id;
        BeginDrawing(); DrawChatHistory(bounds); EndDrawing();
        check(chatMessages[chatMessages.size()-1].id == appliedLast);
        check(chatMessages.size() == chatLayout.blocks.size());
        check(appliedLast > cursor || !browsingHistory);
    }
    check(chatMessages[chatMessages.size()-1].id == 101);
    ClearChatHistory();
    // Force many automatic batch flushes. Drawing order and scissor clipping
    // must survive the smaller batch, including on a larger Retina window.
    SetWindowSize(800,600);
    BeginDrawing(); ClearBackground(RAYWHITE);
    BeginScissorMode(40,40,720,520);
    for (int i = 0; i < 10000; ++i)
        DrawRectangle(50+(i%100)*7,50+(i/100)*5,6,4,BLUE);
    DrawRectangle(100,100,100,100,RED);
    DrawClientText("Sodium React - café",120,120,16,BLACK);
    EndScissorMode();
    Image frame = LoadImageFromScreen();
    const float scaleX = static_cast<float>(frame.width)/GetScreenWidth();
    const float scaleY = static_cast<float>(frame.height)/GetScreenHeight();
    auto pixel = [&](int x, int y) { return GetImageColor(frame, static_cast<int>(x*scaleX), static_cast<int>(y*scaleY)); };
    const auto red = pixel(110,110), outside = pixel(20,20), blue = pixel(52,52);
    check(red.r == RED.r && red.g == RED.g && red.b == RED.b);
    check(outside.r == RAYWHITE.r && outside.g == RAYWHITE.g && outside.b == RAYWHITE.b);
    check(blue.r == BLUE.r && blue.g == BLUE.g && blue.b == BLUE.b);
    UnloadImage(frame); EndDrawing();
    GuiSetFont(GetFontDefault());
    if (headingFont.texture.id != GetFontDefault().texture.id) UnloadFont(headingFont);
    if (messageFont.texture.id != GetFontDefault().texture.id) UnloadFont(messageFont);
    CloseWindow();
}
