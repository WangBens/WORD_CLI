#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <cjson/cJSON.h>

#define MAX_INPUT 1024
#define MAX_TOOLS 50
#define API_ENDPOINT "https://integrate.api.nvidia.com/v1/chat/completions"

typedef struct {
    char name[256];
    char qty[64];
    char note[256];
} Tool;

typedef struct {
    char objectives[2048];
    char theory[2048];
    char steps_draft[2048];
    char discussion[2048];
    char exp_wang[1024];
    char exp_liao[1024];
    char exp_chen[1024];
} Drafts;

typedef struct {
    char *data;
    size_t size;
} ResponseBuffer;

/* ==========================================
 * 自訂的 .env 讀取器 (專門抓 NVIDIA_API_KEY)
 * ========================================== */
char* load_api_key_from_env() {
    FILE *file = fopen(".env", "r");
    if (!file) {
        return NULL; // 找不到檔案
    }

    static char api_key[256];
    char line[512];

    while (fgets(line, sizeof(line), file)) {
        // 清除行尾的換行符號 (\r 或 \n)
        line[strcspn(line, "\r\n")] = 0;

        // 找尋 NVIDIA_API_KEY= 開頭的設定
        if (strncmp(line, "NVIDIA_API_KEY=", 15) == 0) {
            strcpy(api_key, line + 15); // 把等號後面的字串複製出來
            fclose(file);
            return api_key;
        }
    }
    fclose(file);
    return NULL; // 檔案裡沒寫 NVIDIA_API_KEY
}

/* cURL 回調函數 (處理 API 回傳的資料串流) */
static size_t WriteCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    ResponseBuffer *mem = (ResponseBuffer *)userp;
    char *ptr = realloc(mem->data, mem->size + realsize + 1);
    if (!ptr) {
        printf("記憶體不足\n");
        return 0;
    }
    mem->data = ptr;
    memcpy(&(mem->data[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->data[mem->size] = 0;
    return realsize;
}

/* 呼叫 NVIDIA API */
char* callNVIDIAAPI(const char *prompt, const char *api_key) {
    CURL *curl;
    CURLcode res;
    ResponseBuffer buffer = {0};
    buffer.data = malloc(1);
    buffer.data[0] = '\0';

    curl = curl_easy_init();
    if (!curl) {
        printf("cURL 初始化失敗\n");
        free(buffer.data);
        return NULL;
    }

    /* 構建 JSON 請求 */
    cJSON *json = cJSON_CreateObject();
    cJSON *messages = cJSON_CreateArray();
    
    cJSON *sys_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(sys_msg, "role", "system");
    cJSON_AddStringToObject(sys_msg, "content", "你是一個嚴格的 JSON 輸出機器。只回傳合法 JSON，不要有任何 markdown、解釋或額外文字。");
    cJSON_AddItemToArray(messages, sys_msg);
    
    cJSON *user_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON_AddStringToObject(user_msg, "content", prompt);
    cJSON_AddItemToArray(messages, user_msg);
    
    cJSON_AddItemToObject(json, "messages", messages);
    
    /* 使用穩定的模型 */
    cJSON_AddStringToObject(json, "model", "meta/llama-3.1-70b-instruct");
    cJSON_AddNumberToObject(json, "temperature", 0.2);
    cJSON_AddNumberToObject(json, "max_tokens", 4096);

    char *json_str = cJSON_PrintUnformatted(json);
    
    /* 設置 HTTP Headers 與 API Key */
    char auth_header[512];
    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", api_key);
    
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, auth_header);
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, API_ENDPOINT);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_str);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&buffer);

    /* 執行請求 */
    printf(">>> 正在調用 NVIDIA NIM API...\n");
    res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        printf("!!! cURL 執行失敗 (可能是沒有網路): %s\n", curl_easy_strerror(res));
        free(json_str);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        free(buffer.data);
        cJSON_Delete(json);
        return NULL;
    }

    /* HTTP 狀態碼防護網 */
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    
    if (http_code != 200) {
        printf("\n!!! 伺服器拒絕請求，HTTP 狀態碼: %ld !!!\n", http_code);
        if (http_code == 401) {
            printf("=> ❌ 你的 API KEY 無效、寫錯，或者免費額度已經用盡了！\n");
        } else if (http_code == 404) {
            printf("=> ❌ 模型名稱錯誤！伺服器上找不到這個語言模型。\n");
        } else {
            printf("=> ❌ 發生其他格式或網路參數錯誤。\n");
        }
        
        if (strlen(buffer.data) > 0) {
            printf("=> 伺服器補充說明: %s\n", buffer.data);
        }
        
        free(json_str);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        free(buffer.data);
        cJSON_Delete(json);
        return NULL; 
    }

    printf(">>> API 成功回傳，開始解析...\n");

    /* 清理 */
    free(json_str);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    cJSON_Delete(json);

    return buffer.data;
}

/* 從 API 回傳提取 JSON 內容 */
cJSON* parseAPIResponse(const char *response) {
    if (!response) return NULL;

    cJSON *root = cJSON_Parse(response);
    if (!root) {
        printf("!!! JSON 解析失敗 (伺服器回傳的不是合法的 JSON 格式)\n");
        return NULL;
    }

    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    if (!choices || !cJSON_IsArray(choices) || cJSON_GetArraySize(choices) == 0) {
        printf("!!! API 回傳格式異常\n");
        cJSON_Delete(root);
        return NULL;
    }

    cJSON *first_choice = cJSON_GetArrayItem(choices, 0);
    cJSON *message = cJSON_GetObjectItem(first_choice, "message");
    cJSON *content = cJSON_GetObjectItem(message, "content");

    if (!content || !cJSON_IsString(content)) {
        printf("!!! 無法提取內容\n");
        cJSON_Delete(root);
        return NULL;
    }

    cJSON *result = cJSON_Parse(content->valuestring);
    cJSON_Delete(root);
    return result;
}

/* 蒐集輸入資訊 */
void gatherInput(char *lab_date, char *lab_name, Tool *tools, int *tool_count, Drafts *drafts) {
    printf("\n=== 物理實驗報告自動化系統 (.env 安全版) ===\n\n");
    
    printf("請輸入實驗日期 (預設 2026/4/8)：");
    fgets(lab_date, MAX_INPUT, stdin);
    lab_date[strcspn(lab_date, "\n")] = 0;
    if (strlen(lab_date) == 0) strcpy(lab_date, "2026/4/8");
    
    printf("請輸入實驗名稱：");
    fgets(lab_name, MAX_INPUT, stdin);
    lab_name[strcspn(lab_name, "\n")] = 0;
    if (strlen(lab_name) == 0) strcpy(lab_name, "實驗4 電力線之分布");
    
    printf("\n--- 填寫材料與儀器 (名稱留空結束) ---\n");
    *tool_count = 0;
    while (*tool_count < MAX_TOOLS) {
        printf("儀器名稱：");
        fgets(tools[*tool_count].name, 256, stdin);
        tools[*tool_count].name[strcspn(tools[*tool_count].name, "\n")] = 0;
        
        if (strlen(tools[*tool_count].name) == 0) break;
        
        printf("[%s] 的數量：", tools[*tool_count].name);
        fgets(tools[*tool_count].qty, 64, stdin);
        tools[*tool_count].qty[strcspn(tools[*tool_count].qty, "\n")] = 0;
        
        printf("[%s] 的備註：", tools[*tool_count].name);
        fgets(tools[*tool_count].note, 256, stdin);
        tools[*tool_count].note[strcspn(tools[*tool_count].note, "\n")] = 0;
        
        (*tool_count)++;
    }
    
    printf("\n--- 填寫實驗內文草稿 ---\n");
    
    printf("0. 實驗目的草稿：");
    fgets(drafts->objectives, sizeof(drafts->objectives), stdin);
    drafts->objectives[strcspn(drafts->objectives, "\n")] = 0;
    
    printf("1. 實驗原理草稿：");
    fgets(drafts->theory, sizeof(drafts->theory), stdin);
    drafts->theory[strcspn(drafts->theory, "\n")] = 0;
    
    printf("2. 步驟/方法草稿：");
    fgets(drafts->steps_draft, sizeof(drafts->steps_draft), stdin);
    drafts->steps_draft[strcspn(drafts->steps_draft, "\n")] = 0;
    
    printf("3. 結果與討論草稿：");
    fgets(drafts->discussion, sizeof(drafts->discussion), stdin);
    drafts->discussion[strcspn(drafts->discussion, "\n")] = 0;
    
    printf("王同學心得：");
    fgets(drafts->exp_wang, sizeof(drafts->exp_wang), stdin);
    drafts->exp_wang[strcspn(drafts->exp_wang, "\n")] = 0;
    
    printf("廖同學心得：");
    fgets(drafts->exp_liao, sizeof(drafts->exp_liao), stdin);
    drafts->exp_liao[strcspn(drafts->exp_liao, "\n")] = 0;
    
    printf("陳同學心得：");
    fgets(drafts->exp_chen, sizeof(drafts->exp_chen), stdin);
    drafts->exp_chen[strcspn(drafts->exp_chen, "\n")] = 0;
}

/* 建構 AI 提示語 */
void buildPrompt(char *prompt, size_t max_size, const Drafts *drafts) {
    snprintf(prompt, max_size,
        "你現在是一位正在寫物理實驗報告的大學生。\n"
        "請幫我\"一次全部\"改寫以下草稿，並嚴格依照指定的 JSON 格式回傳。\n\n"
        "【修改要求】\n"
        "1. 語氣白話自然，像一般大學生寫的報告，修正錯字與不通順的語句。\n"
        "2. objectives (實驗目的)：請改寫成有條理的段落。\n"
        "3. method (實驗方法)：將步驟草稿彙整成\"一段\"通順流暢的段落，不要編號。\n"
        "4. steps (實驗步驟)：將步驟草稿整理成\"1. 2. 3.\"的清單格式，每點獨立一行（在 JSON 中請使用 \\\\n 換行符號來分行）。\n"
        "5. discussion (結果與討論)：包含誤差分析，通順即可。\n"
        "6. 三位組員的心得：以各自的第一人稱改寫，保持誠懇。\n\n"
        "【輸入草稿】\n"
        "實驗目的：%s\n實驗原理：%s\n步驟方法：%s\n結果討論：%s\n"
        "王心得：%s\n廖心得：%s\n陳心得：%s\n\n"
        "【必須輸出的 JSON 格式】\n"
        "{\n"
        "    \"objectives\": \"修改後的實驗目的\",\n"
        "    \"theory\": \"修改後的實驗原理\",\n"
        "    \"method\": \"修改後的實驗方法(段落)\",\n"
        "    \"steps\": \"1. 第一步\\\\n2. 第二步\\\\n3. 第三步\",\n"
        "    \"discussion\": \"修改後的結果與討論\",\n"
        "    \"exp_wang\": \"修改後的王同學心得\",\n"
        "    \"exp_liao\": \"修改後的廖同學心得\",\n"
        "    \"exp_chen\": \"修改後的陳同學心得\"\n"
        "}",
        drafts->objectives, drafts->theory, drafts->steps_draft, drafts->discussion,
        drafts->exp_wang, drafts->exp_liao, drafts->exp_chen
    );
}

/* 調用 Python 腳本生成 DOCX */
void generateDocx(const char *lab_date, const char *lab_name, const Tool *tools, int tool_count, const cJSON *refined_data) {
    printf("\n--- 正在生成 WORD 文件 ---\n");
    
    FILE *py_file = fopen("generate_report.py", "w");
    if (!py_file) {
        printf("!!! 無法創建臨時 Python 腳本\n");
        return;
    }
    
    fprintf(py_file, "#!/usr/bin/env python3\n");
    fprintf(py_file, "import sys\nimport json\nfrom docxtpl import DocxTemplate\nfrom docx2pdf import convert\n\n");
    fprintf(py_file, "template_path = '%s'\n", "template.docx");
    fprintf(py_file, "try:\n");
    fprintf(py_file, "    doc = DocxTemplate(template_path)\n");
    fprintf(py_file, "except Exception as e:\n");
    fprintf(py_file, "    print(f'讀取模板失敗，請確認 template.docx 存在: {e}')\n");
    fprintf(py_file, "    sys.exit(1)\n\n");
    
    /* 準備 context */
    fprintf(py_file, "context = {\n");
    fprintf(py_file, "    'lab_date': '%s',\n", lab_date);
    fprintf(py_file, "    'lab_name': '%s',\n", lab_name);
    
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, refined_data) {
        if (cJSON_IsString(item)) {
            fprintf(py_file, "    '%s': \"\"\"%s\"\"\",\n", item->string ? item->string : "unknown", item->valuestring);
        }
    }
    
    fprintf(py_file, "}\n\n");
    fprintf(py_file, "doc.render(context)\n");
    
    /* 添加工具表格 */
    fprintf(py_file, "for table in doc.docx.tables:\n");
    fprintf(py_file, "    if '名稱' in table.cell(0, 0).text:\n");
    for (int i = 0; i < tool_count; i++) {
        fprintf(py_file, "        row_cells = table.add_row().cells\n");
        fprintf(py_file, "        row_cells[0].text = '%s'\n", tools[i].name);
        fprintf(py_file, "        row_cells[1].text = '%s'\n", tools[i].qty);
        fprintf(py_file, "        row_cells[2].text = '%s'\n", tools[i].note);
    }
    fprintf(py_file, "        break\n\n");
    
    fprintf(py_file, "output_docx = '實驗報告_成品_穩定版.docx'\n");
    fprintf(py_file, "doc.save(output_docx)\n");
    fprintf(py_file, "print(f'成功產出：{output_docx}')\n\n");
    
    fprintf(py_file, "try:\n");
    fprintf(py_file, "    output_pdf = '實驗報告_成品_穩定版.pdf'\n");
    fprintf(py_file, "    convert(output_docx, output_pdf)\n");
    fprintf(py_file, "    print(f'成功產出：{output_pdf}')\n");
    fprintf(py_file, "except Exception as e:\n");
    fprintf(py_file, "    print(f'PDF 轉檔失敗 (可能是環境不支援 docx2pdf)：{e}')\n");
    
    fclose(py_file);
    
    /* 執行 Python 腳本 */
    printf(">>> 執行 Python 腳本生成 WORD 文件...\n");
    int ret = system("python generate_report.py");
    
    if (ret == 0) {
        printf("✓ 文件生成流程結束！\n");
    } else {
        printf("!!! Python 腳本執行過程發生錯誤 (請確認是否有安裝相關套件)\n");
    }
    
    remove("generate_report.py");
}

int main() {
    /* 1. 安全性檢查：從 .env 讀取 API Key */
    char *api_key = load_api_key_from_env();
    if (!api_key || strlen(api_key) == 0) {
        printf("===================================================\n");
        printf("❌ 錯誤：找不到 .env 檔案，或是裡面沒有設定 NVIDIA_API_KEY！\n");
        printf("👉 請在與這個 exe 同一個資料夾下，建立一個叫做 .env 的文字檔\n");
        printf("👉 並在裡面寫入：NVIDIA_API_KEY=你的金鑰\n");
        printf("===================================================\n\n");
        system("pause");
        return 1;
    }

    char lab_date[MAX_INPUT];
    char lab_name[MAX_INPUT];
    Tool tools[MAX_TOOLS];
    int tool_count = 0;
    Drafts drafts;
    
    memset(lab_date, 0, MAX_INPUT);
    memset(lab_name, 0, MAX_INPUT);
    memset(&drafts, 0, sizeof(Drafts));
    
    /* 動態分配記憶體給 prompt，防止緩衝區溢位 */
    size_t prompt_size = 32768; 
    char *prompt = malloc(prompt_size);
    if (!prompt) {
        printf("錯誤：記憶體分配失敗！\n");
        system("pause");
        return 1;
    }
    memset(prompt, 0, prompt_size);
    
    /* 2. 蒐集輸入 */
    gatherInput(lab_date, lab_name, tools, &tool_count, &drafts);
    
    /* 3. 構建提示語 */
    buildPrompt(prompt, prompt_size, &drafts);
    
    /* 4. 調用 API */
    char *api_response = callNVIDIAAPI(prompt, api_key);
    free(prompt);
    
    if (!api_response) {
        printf("!!! API 調用失敗或遭到拒絕\n");
        system("pause");
        return 1;
    }
    
    /* 5. 解析回傳 */
    cJSON *refined_data = parseAPIResponse(api_response);
    free(api_response);
    
    if (!refined_data) {
        printf("!!! 無法解析 API 回傳\n");
        system("pause");
        return 1;
    }
    
    printf("\n=== 🕵️ AI 實際回傳的內容檢查 ===\n");
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, refined_data) {
        if (cJSON_IsString(item)) {
            printf("標籤 [%s] 裡面有 %zu 個字\n", item->string ? item->string : "unknown", strlen(item->valuestring));
        }
    }
    printf("==================================\n");
    
    /* 6. 生成 DOCX */
    generateDocx(lab_date, lab_name, tools, tool_count, refined_data);
    
    cJSON_Delete(refined_data);
    printf("\n✓ 程式執行完成！\n\n");
    
    system("pause");
    return 0;
}