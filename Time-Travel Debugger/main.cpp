// ======================= TIME-TRAVEL DEBUGGER - SERVER TEMPLATE =======================

// Pipeline this file implements, top to bottom:
//   0. Receive  -- stream the client's .trace bytes straight to source.bin on disk
//   1. Pass 0X0   -- validity check (FUNC/FUNC_END matching)
//   2. Pass 0X1   -- resolve(): copy EVERY source line into resolve.bin as [offset][size][string], then patch CALL targets.
//   3. Pass 0X2   -- execute resolve.bin: tokenize ONE line at a time, update the call stack, take a snapshot -> Timeline
//   4. Pass 0X3   -- serialize Timeline -> session.tdbg(header + snapshot records + dense index)

/*
func foo b
set a 10
add b a
func_end
func main
set k 10
call foo k
func_end
*/

#include <iostream>
#include <string>
#include <cstdint>
#include <fstream>
#ifdef _WIN32
    #include <ws2tcpip.h>
    #include <winsock2.h>
#else
    #include <unistd.h>
    #include <sys/socket.h>
#endif
#include <cstdio>
using namespace std;

// ---- Constants ----
const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
const int32_t MAX_FUNCS = 128;
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2; // kW + func_name + upto 16 params/args
const int32_t MAX_PATCHES = MAX_FUNCS * 4;
const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024; // sanity cap on the declared file length
const int32_t IO_BUFFER_SIZE = 64 * 1024;                  // fixed buffer for streaming to/from disk
const int32_t SOCKET_TIMEOUT_SEC = 5;                      // TODO: apply as SO_RCVTIMEO so a deadclient can't hang the server forever

// ---- Custom data structures

// Stack: back the live Call Stack during execution
template <typename T>
class Stack
{
    struct Node
    {
        T data;
        Node *next;

        Node(const T& d, Node* ptr) :data(d), next(ptr) {}
    };
    Node *top;
    int32_t count; 

public:
    // Implement these functions:

	Stack():top(nullptr),count(0){}

    void push(const T &val)
    {
		if(count<=0){
			top=new Node(val,nullptr);
            count++;
            return;
        }
        if(count<=MAX_STACK_DEPTH){
            top=new Node(val,top);
            count++;
        }
        else {
            throw "Stack overflow";
        }
    }
    T pop()
    {
        if (count > 0) {
            Node* a = top->next;
            T value = top->data;
            delete top;
            top = a;
            count--;
            return value;
        }
        else {
            throw "Stack underflow";
        }
    }
    
    T &peek()
    {
        if(count>0) return top->data;
        throw "Stack underflow";
    }
    bool isEmpty()
    {
        if(count<=0)return 1;
        return 0;
    }
    int32_t depth()
    {
        return count;
    }
    int32_t snapshot_into(T out[], int32_t maxLen)
    {
        // copies every frame, top to bottom in the array given as a parameter
        // this is what buildSnapshot() call, returns count written
        Node* temp=top;
        for(int i=0;i<maxLen&&i<count;i++){
            out[i]=temp->data;
            temp=temp->next;
        }
        if(count<=maxLen)return count;
        return maxLen;
    }
};


// Timeline : doubly linked list of Snapshots
struct Snapshot; // fwd declaration;
struct TimelineNode
{
    Snapshot *data;
    TimelineNode *next;
    TimelineNode *prev;

    TimelineNode(Snapshot*s):data(s),next(nullptr),prev(nullptr){}
};

class Timeline
{
    TimelineNode *head, *tail;
    int32_t stepCount;

public:
    // Implement these functions
    Timeline():head(nullptr),tail(nullptr),stepCount(0){}
    void record(Snapshot *s)
    {
        if (head == nullptr) {
           head = tail = new TimelineNode(s);
           stepCount++;
           return;
        }
        if(head==tail){
            tail=new TimelineNode(s);
            head->next=tail;
            tail->prev=head;
            stepCount++;
            return;
        }
        TimelineNode* temp = tail;
        tail = new TimelineNode(s);
        temp->next = tail;
        tail->prev=temp;
        stepCount++;
    }
    TimelineNode *begin()
    {
        return head;
    }
    int32_t getStepCount()
    {
        return stepCount;
    }
};

// Core structs
struct Variable
{
    string name;
    int32_t value;
};
struct Frame
{
    string func_name;
    int32_t argc;
    Variable argv[MAX_VARS_PER_FRAME];
    int64_t returnLine;
    Variable locals[MAX_VARS_PER_FRAME];
    int32_t localCount;
    Frame():func_name(""),argc(0),argv{},returnLine(-1),locals{},localCount(0){}

    Frame(string name,int32_t argC=0,int64_t offset=-1,int32_t localC=0):func_name(name),argc(argC),returnLine(offset),localCount(localC){}
};
struct Snapshot
{
    Frame callStack[MAX_STACK_DEPTH];
    int32_t stackDepth;
};
struct TTDBHeader
{
    char magic[4]; // "TTDB"
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;

    TTDBHeader ():version(1),stepCount(0),indexOffset(0) {
        magic[0]='T';
        magic[1]='T';
        magic[2]='D';
        magic[3]='B';
        
    }
    

};
void writeHeader(FILE *f, const TTDBHeader &h)
{
    fwrite(h.magic, 1, 4, f);
    fwrite(&h.version, sizeof(int32_t), 1, f);
    fwrite(&h.stepCount, sizeof(int32_t), 1, f);
    fwrite(&h.indexOffset, sizeof(int64_t), 1, f);

    // placeholder for other two data members
}

// resolve.bin - bookkeeping
struct FuncEntry
{
    string funcName;
    int64_t byteOffsetInResolveBin; // where this function's FUNC header record sits
};
struct PendingPatch
{
    int64_t byteOffsetOfOffsetField; // where in resolve.bin to seek back and overwrite
    string targetFuncName;
};



// PASS 0x0: READING source.bin + VALIDITY CHECK
bool readSourceLine(ifstream &in, string &out)
{
   do{
        if(getline(in,out)&&!(out==""))
            return 1;
    } while(out==""&&!in.eof());
    return 0;
}
string firstWord(const string &line)
{
    string word;
    for(int i =0;line[i]!=' '&&line[i]!=0&&line[i]!='\t';i++)word+=line[i];
    return word;
    
}
string secondWord(const string &line)
{
    string word2;
    int i=0;
    while(line[i]!=' '&&line[i]!=0&&line[i]!='\t'){
        i++;
    }
    if(line[i]==0)return "";
    i++;
    while(line[i]!=' '&&line[i]!=0&&line[i]!='\t'){
        word2+=line[i];
        i++;
    }
    return word2;
}
bool validateProgram(const char *sourcePath)
{
    ifstream in(sourcePath);
    bool flag=0;
    string w1;
    string w2;
    while(!in.eof()){
        string line;
        if(!readSourceLine(in,line))break;
        
        w1=firstWord(line);
        
        if(w1=="func"&&(!flag)) {
            flag=1;
            w2=secondWord(line);
            if(w2=="")return 0;
        }
        else if(w1=="func"&& flag) return 0;
        else if(w1=="func_end"&&(!flag))return 0;
        if(w1=="func_end"&&flag){
            flag=0;
        }

    }
    if(flag)return 0;
    return 1;
    // for each func defined there should be exactly one func_end and no nested funcs allowed - 
}

// PASS 0x1: RESOLVE() -> resolve.bin
int64_t writeResolveRecord(FILE *f, int64_t offsetField, const string &text)
{
    int64_t _offset=ftell(f);
    int32_t sizeString=text.size();
    fwrite(&offsetField,sizeof(int64_t),1,f);
    fwrite(&sizeString,sizeof(int32_t),1,f);
    for(int i=0;i<text.size();i++){
        fwrite(&text[i],sizeof(char),1,f);
    }
    return _offset;
    // writes one [offset(8B)][size(4B)][string] record at the current file position
    // returns this record's own starting byte position
}
int64_t readResolveRecord(FILE *f, string &outText)
{
    int64_t offset;
    int32_t size;
    fread(&offset,sizeof(int64_t),1,f);
    fread(&size,sizeof(int32_t),1,f);
    char t;
    outText="";
    for(int32_t i =0;i<size;i++){
        fread(&t,sizeof(char),1,f);
        outText+=t;
    }
    return offset;
    // reads one record at the current position and advances past it, returns the offset field - the raw line text comes back untouched in outText.
}
int64_t resolveProgram(const char *sourcePath, const char *resolveBinPath)
{
    FuncEntry funcArray[MAX_FUNCS];
    int32_t funcCount = 0;
    PendingPatch patches[MAX_PATCHES];
    int32_t patchCount = 0;

    int64_t offsetField=0;
    int64_t mainOffset=0;

    ifstream in(sourcePath);
    if(!in){
        throw "source file not found";
    }

    FILE* out=fopen(resolveBinPath,"wb");
    if(!out){
        throw "resolve file not found";
    }
    
    string line,w1,w2;
    bool mainFlag=0;
    while(!in.eof()){
        if(!readSourceLine(in,line))break;
        
        w1=firstWord(line);
        w2=secondWord(line);
        
        if(w1=="func"){
            if(w2=="main"&&mainFlag==0){
                mainFlag=1;
                mainOffset=offsetField;
            }else if(w2=="main"&&mainFlag)throw "Repeating main";
            funcArray[funcCount].funcName=secondWord(line);
            funcArray[funcCount++].byteOffsetInResolveBin=offsetField;
            if(funcCount>MAX_FUNCS)throw "Exceeded maxFuncton limit";
            int64_t _offset=writeResolveRecord(out,offsetField,line);    
            offsetField+=8+4+line.size();            
        }
        else if(w1=="call"){
            bool f=0;
            for(int i=0;i<funcCount;i++){
                if(w2==funcArray[i].funcName){
                    int64_t _offset=writeResolveRecord(out,funcArray[i].byteOffsetInResolveBin,line);    
                    offsetField+=8+4+line.size();
                    f=1;
                    break;
                }
            }
            if(f)continue;
            patches[patchCount].targetFuncName=w2;
            patches[patchCount++].byteOffsetOfOffsetField=offsetField;
            if(patchCount>MAX_PATCHES)throw "Exceeded maxPatch limit";
            int64_t _offset=writeResolveRecord(out,offsetField,line);    
            offsetField+=8+4+line.size();
            
        }
        else{
            int64_t _offset=writeResolveRecord(out,offsetField,line);    
            offsetField+=8+4+line.size();
        }
    }
    //main check
    if(!mainFlag)throw "main not found";
    //patching
    bool flag=0;
    for(int i=0;i<patchCount;i++){
        for(int j=0;j<funcCount;j++){
            if(patches[i].targetFuncName==funcArray[j].funcName){
                fseek(out,patches[i].byteOffsetOfOffsetField,SEEK_SET);
                fwrite(&funcArray[j].byteOffsetInResolveBin,sizeof(int64_t),1,out);
                flag=1;
                break;
            }
        }
        if(flag)flag=0;
        else throw "undeclared function called";


    }

    

    fclose(out);
    

    return mainOffset;

    // Every source line becomes one record holding the raw line, as-is.
    // resolve() only PEEKS at the leading word(s) -- enough to spot FUNC
    // (remember its position) and CALL (remember which function it needs
    // and where its offset field sits).
    // Once the whole file is written, every CALL's offset field is patched
    // with its target's position. Patching happens after the full write
    // Returns the byte offset of main's FUNC header record.
    // if there is no main return the error 
}

// PASS 0x2: EXECUTION (tokenization happens here)
enum tokenType
{
    KEYWORD,
    IDENTIFIER,
    PARAM
};
struct Token
{
    tokenType type;
    string text;
};
int32_t tokenizeLine(const string &line, Token tokens[], int32_t maxTokens)
{
    int i=0;
    int count=0;
    string word="";
    if(count>=maxTokens)return count;
    while(line[i]==' ')i++;
    while(line[i]!=' '&&line[i]!=0)word+=line[i++];
    tokens[count].type=KEYWORD;
    tokens[count].text=word;
    count++;

    if(word=="func_end")return count;
    
    word="";
    if(count>=maxTokens)return count;
    while(line[i]==' ')i++;
    while(line[i]!=' '&&line[i]!=0)word+=line[i++];
    tokens[count].type=IDENTIFIER;
    tokens[count].text=word;
    count++;
    while(line[i]!=0){
        word="";
        while(line[i]==' ')i++;
        if(line[i]==0)break;
        if(count>=maxTokens)throw "max Token already reached";
        while(line[i]!=' '&&line[i]!=0)word+=line[i++];
        tokens[count].type=PARAM;
        tokens[count].text=word;
        count++;
    }
    return count;
    // first word is always a instruction keyword
    // instruction set = [func, func_end, call, set, add, sub, mul and div]
    // next word is identifier like name of a function, variable name
    // after identifier all are the params/arg, space separated
}
Snapshot *buildSnapshot(Stack<Frame> &callStack)
{
    Snapshot* snapsnap=new Snapshot;
    snapsnap->stackDepth=callStack.snapshot_into(snapsnap->callStack,MAX_STACK_DEPTH);
    return snapsnap;
    // build the snapshot based on the callStack given
}
void executeProgram(const char *resolveBinPath, int64_t mainOffset, Timeline &timeline)
{
    FILE* in = fopen(resolveBinPath,"rb");
    if(in==nullptr)throw "resolve file not found";
    if(fseek(in,mainOffset,SEEK_SET))throw "main not found in resolve file";

    Stack<Frame> callStack;

    Frame mainFrame("main",0,-1,0);
    
    callStack.push(mainFrame);
    
    string line;
    readResolveRecord(in,line);
    cout<<line<<endl;
    
    
    Token tokens[MAX_TOKENS];
    int32_t tokenCount=0;
    while(true){
        int64_t lineOffsetByte = ftell(in);
        int64_t offsetbyte = readResolveRecord(in,line);
        cout<<line<<endl;
        tokenCount = tokenizeLine(line,tokens,MAX_TOKENS);
        string key =tokens[0].text;
        timeline.record(buildSnapshot(callStack));
        if(key=="set"){
            if(tokenCount!=3)throw "wrong set instruction";
            Frame &current=callStack.peek();
            string var=tokens[1].text;
            int32_t value=stoi(tokens[2].text);
            bool flag=0;
            for(int i=0;i<current.argc;i++){
                if(current.argv[i].name==var){
                    current.argv[i].value=value;
                    flag=1;
                    break;
                }
            }
            if(flag)continue;
            for(int i=0;i<current.localCount;i++){
                if(current.locals[i].name==var){
                    current.locals[i].value=value;
                    flag=1;
                    break;
                }
            }
            if(flag)continue;
            
            if(current.localCount>=MAX_VARS_PER_FRAME)throw "cant add more variable";
            current.locals[current.localCount].name=var;
            current.locals[current.localCount++].value=value;
            
        }
        else if(key=="add"){
            if(tokenCount!=3)throw "wrong add instruction";
            Frame &current=callStack.peek();
            string var1=tokens[1].text;
            string var2=tokens[2].text;
            bool flag1=0;
            bool flag2=0;
            Variable* Var1;
            Variable* Var2;
            for(int i=0;i<current.argc;i++){
                if(current.argv[i].name==var1){
                    flag1=1;
                    Var1= &current.argv[i];
                }
                if(current.argv[i].name==var2){
                    Var2= &current.argv[i];
                    flag2=1;
                }
                if(flag1&&flag2)break;
            }
            if(flag1&&flag2){
                Var1->value+=Var2->value;
                continue;
            }
            for(int i=0;i<current.localCount;i++){
                if(current.locals[i].name==var1){
                    Var1= &current.locals[i];
                    flag1=1;
                }
                if(current.locals[i].name==var2){
                    Var2= &current.locals[i];
                    flag2=1;
                }
                if(flag1&&flag2)break;
            }
             if(flag1&&flag2){
                Var1->value+=Var2->value;
                continue;
            }
             if(!(flag1&&flag2)){
                throw "Variabes not found";
                
            }

        }
        else if(key=="sub"){
            if(tokenCount!=3)throw "wrong sub instruction";
            Frame &current=callStack.peek();
            string var1=tokens[1].text;
            string var2=tokens[2].text;
            bool flag1=0;
            bool flag2=0;
            Variable* Var1;
            Variable* Var2;
            for(int i=0;i<current.argc;i++){
                if(current.argv[i].name==var1){
                    flag1=1;
                    Var1= &current.argv[i];
                }
                if(current.argv[i].name==var2){
                    Var2= &current.argv[i];
                    flag2=1;
                }
                if(flag1&&flag2)break;
            }
            if(flag1&&flag2){
                Var1->value-=Var2->value;
                continue;
            }
            for(int i=0;i<current.localCount;i++){
                if(current.locals[i].name==var1){
                    Var1= &current.locals[i];
                    flag1=1;
                }
                if(current.locals[i].name==var2){
                    Var2= &current.locals[i];
                    flag2=1;
                }
                if(flag1&&flag2)break;
            }
             if(flag1&&flag2){
                Var1->value-=Var2->value;
                continue;
            }
             if(!(flag1&&flag2)){
                throw "Variabes not found";
                
            }

        }
        else if(key=="mul"){
            if(tokenCount!=3)throw "wrong add instruction";
            Frame &current=callStack.peek();
            string var1=tokens[1].text;
            string var2=tokens[2].text;
            bool flag1=0;
            bool flag2=0;
            Variable* Var1;
            Variable* Var2;
            for(int i=0;i<current.argc;i++){
                if(current.argv[i].name==var1){
                    flag1=1;
                    Var1= &current.argv[i];
                }
                if(current.argv[i].name==var2){
                    Var2= &current.argv[i];
                    flag2=1;
                }
                if(flag1&&flag2)break;
            }
            if(flag1&&flag2){
                Var1->value*=Var2->value;
                continue;
            }
            for(int i=0;i<current.localCount;i++){
                if(current.locals[i].name==var1){
                    Var1= &current.locals[i];
                    flag1=1;
                }
                if(current.locals[i].name==var2){
                    Var2= &current.locals[i];
                    flag2=1;
                }
                if(flag1&&flag2)break;
            }
            if(flag1&&flag2){
                Var1->value*=Var2->value;
                continue;
            }
             if(!(flag1&&flag2)){
                throw "Variabes not found";
                
            }

        }
        else if(key=="div"){
            if(tokenCount!=3)throw "wrong add instruction";
            Frame &current=callStack.peek();
            string var1=tokens[1].text;
            string var2=tokens[2].text;
            bool flag1=0;
            bool flag2=0;
            Variable* Var1;
            Variable* Var2;
            for(int i=0;i<current.argc;i++){
                if(current.argv[i].name==var1){
                    flag1=1;
                    Var1= &current.argv[i];
                }
                if(current.argv[i].name==var2){
                    Var2= &current.argv[i];
                    if(current.argv[i].value==0)throw"IllEGAL OPERATION DIVIDING BY ZERO";
                    flag2=1;
                }
                if(flag1&&flag2)break;
            }
            if(flag1&&flag2){
                Var1->value/=Var2->value;
                continue;
            }
            for(int i=0;i<current.localCount;i++){
                if(current.locals[i].name==var1){
                    Var1= &current.locals[i];
                    flag1=1;
                }
                if(current.locals[i].name==var2){
                    Var2= &current.locals[i];
                    if(current.locals[i].value==0)throw"IllEGAL OPERATION DIVIDING BY ZERO";
                    flag2=1;
                }
                if(flag1&&flag2)break;
            }
             if(flag1&&flag2){
                Var1->value/=Var2->value;
                continue;
            }
             if(!(flag1&&flag2)){
                throw "Variabes not found";
                
            }

        }
        else if(key=="func"){
            continue;
        }
        else if(key=="func_end"){
            if(callStack.depth()==1){
                break;
            }
            
            int64_t callOffset=callStack.peek().returnLine;
            Frame poped=callStack.pop();
            Frame &current=callStack.peek();

            if(fseek(in,callOffset,SEEK_SET)) throw"cant go back to caller";

            offsetbyte = readResolveRecord(in,line);
            tokenCount = tokenizeLine(line,tokens,MAX_TOKENS);
            
            for(int i=0;i<poped.argc;i++){
                bool flag=0;
                for(int j=0;j<current.argc;j++){
                    if(current.argv[j].name==tokens[i+2].text){
                        current.argv[j].value=poped.argv[i].value;
                        flag=1;
                        break;
                        
                    }
                }
                if(flag)continue;
                for(int j=0;j<current.localCount;j++){
                    if(current.locals[j].name==tokens[i+2].text){
                        current.locals[j].value=poped.argv[i].value;
                        flag=1;
                        break;
                        
                    }
                }
                if(!flag)throw "Call argument not found";
            
            }
            
            
        }
        else if(key=="call"){
            if(tokenCount<2) throw "invalid instruction";
            if(tokenCount-2>MAX_VARS_PER_FRAME)throw "can add more variable than the max cap";
            int32_t newArgV[MAX_VARS_PER_FRAME];
            int32_t newArgCount=0;
            bool flag=0;
            
            for(int i=0;i<tokenCount-2;i++){
                flag=0;
                string tempVar=tokens[i+2].text;
                Frame &current=callStack.peek();
                for(int j=0;j<current.argc;j++){
                    if(current.argv[j].name==tempVar){
                        newArgV[newArgCount++]=current.argv[j].value;
                        flag=1;
                        break;
                    }
                }
                if(flag)continue;
                for(int j=0;j<current.localCount;j++){
                    if(current.locals[j].name==tempVar){
                        newArgV[newArgCount++]=current.locals[j].value;
                        flag=1;
                        break;
                    }
                }
                if(!flag)throw "Call argument not found";
            }
            
            int32_t previousCallTokenCount=tokenCount;
            
            
            if(fseek(in,offsetbyte,SEEK_SET))throw "function not found";
            offsetbyte = readResolveRecord(in,line);
            tokenCount=tokenizeLine(line,tokens,MAX_TOKENS);
            if(previousCallTokenCount!=tokenCount)throw"call paramerters dont match function parameter";
            
            Frame temp(tokens[1].text,tokenCount-2,lineOffsetByte,0);
            
            for(int i = 0; i<tokenCount-2;i++){
                temp.argv[i].name=tokens[i+2].text;
                temp.argv[i].value=newArgV[i];
            }
            callStack.push(temp);
        }
        else{
            throw "Wrong keyword used";
        }




    }




}

// PASS 0x3: SERIALIZE TIMELINE
void writeTdbg(Timeline &timeline, const char *tdbgPath)
{
    TTDBHeader header;
    
    int32_t stepcount =timeline.getStepCount();
    header.stepCount=stepcount;
    
    FILE* ttdb = fopen(tdbgPath,"wb");
    if(!ttdb) throw "sessoin file not found";

    writeHeader(ttdb,header);

    // placeholder for header
    int64_t *idx = new int64_t [stepcount]; 
    
    // index array of the size of stepcount from the timeline
    TimelineNode *traversePtr = timeline.begin();
    // placing each snapshot in the file while maintaining the index(starting point of each nth snapshot)
    for(int i=0;i<stepcount;i++){
        if(traversePtr==nullptr) throw"timeline traverse failed";
        idx[i]=ftell(ttdb);

        fwrite(&traversePtr->data->stackDepth,sizeof(int32_t),1,ttdb);

        for(int j=0; j<traversePtr->data->stackDepth;j++){
            
            Frame & tempFrame=traversePtr->data->callStack[j];
            int32_t tempSize;
            
            tempSize=static_cast<int32_t>(tempFrame.func_name.size());
            fwrite(&tempSize,sizeof(int32_t),1,ttdb);
            fwrite(tempFrame.func_name.data(),sizeof(char),tempSize,ttdb);
            fwrite(&tempFrame.argc,sizeof(int32_t),1,ttdb);

            for(int k=0;k<tempFrame.argc;k++){
                tempSize=static_cast<int32_t>(tempFrame.argv[k].name.size());
                fwrite(&tempSize,sizeof(int32_t),1,ttdb);
                fwrite(tempFrame.argv[k].name.data(),sizeof(char),tempSize,ttdb);
                fwrite(&tempFrame.argv[k].value,sizeof(int32_t),1,ttdb);
            }
            fwrite(&tempFrame.returnLine,sizeof(int64_t),1,ttdb);
            fwrite(&tempFrame.localCount,sizeof(int32_t),1,ttdb);

            for(int k=0;k<tempFrame.localCount;k++){
                tempSize=static_cast<int32_t>(tempFrame.locals[k].name.size());
                fwrite(&tempSize,sizeof(int32_t),1,ttdb);
                fwrite(tempFrame.locals[k].name.data(),sizeof(char),tempSize,ttdb);
                fwrite(&tempFrame.locals[k].value,sizeof(int32_t),1,ttdb);
            }
        }    
        traversePtr=traversePtr->next;
        
    }
    header.indexOffset= ftell(ttdb);
    for(int i=0;i<stepcount;i++){
        fwrite(&idx[i],sizeof(int64_t),1,ttdb);
    }
    // after timeline add the index array i the file
    fseek(ttdb,0,SEEK_SET);
    writeHeader(ttdb,header);
    delete [] idx;
    fclose(ttdb);
    // update the header
}
// main section
int32_t main()
{
    try{
        
        cout<<"compile starting"<<endl;
        if (!validateProgram("source.bin"))
        {
            throw "program has nested function or a end function line is present without the start line or no endline for a function";
        }
        int64_t mainOffset = resolveProgram("source.bin", "resolve.bin");

        Timeline timeline;
        executeProgram("resolve.bin", mainOffset, timeline);

        writeTdbg(timeline, "session.tdbg");
    }
    catch (const char* error){
        cerr << "Error: " << error << endl;
        return 1;
    }
    return 0;
}