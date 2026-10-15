#include "library/query.h"
#include <cstdio>
#include <cstdlib>
using namespace z3cloudplay;
static void Check(bool condition,const char* description){
 if(!condition){std::fprintf(stderr,"FAIL: %s\n",description);std::exit(1);}
}
int main(){
 const std::vector<GameSummary> games={
  {20,"Portal 2","","","Valve"}, {10,"portal","","","Valve"},
  {30,"ÉTÉ: L’Aventure!","","","Studio Étoile"},
  {40,"Straße","","","Example"}, {50,"星の旅","","","星工房"},
  {60,"ŁÓDŹ","","","Example"}, {70,"portal","","","Other"}};
 Check(QueryLibrary(games,{},LibrarySort::AppId)==std::vector<GameId>({10,20,30,40,50,60,70}),"empty query returns every supplied game");
 Check(QueryLibrary(games,{"PORTAL"})==std::vector<GameId>({10,70,20}),"case-insensitive Latin search and stable ID tie break");
 Check(QueryLibrary(games,{"été:"})==std::vector<GameId>({30}),"accented Latin casefold retains punctuation");
 Check(QueryLibrary(games,{"strasse"})==std::vector<GameId>({40}),"Latin multi-character casefold");
 Check(QueryLibrary(games,{"łódź"})==std::vector<GameId>({60}),"extended Latin casefold");
 Check(QueryLibrary(games,{"の旅"})==std::vector<GameId>({50}),"non-Latin substring search");
 Check(QueryLibrary(games,{"portal","VALVE"},LibrarySort::TitleDescending)==std::vector<GameId>({20,10}),"developer filter uses real supplied metadata");
 Check(QueryLibrary(games,{"missing"}).empty(),"no results remains empty");
 Check(QueryLibrary(games,{""}).size()==7,"clearing query recovers full library");
 const std::vector<GameSummary> duplicates={{2,"A"},{2,"B"},{0,"Invalid"},{1,"A"}};
 Check(QueryLibrary(duplicates)==std::vector<GameId>({1,2}),"deduplicate valid stable identities");
 auto malformed=games;malformed.push_back({99,std::string("Bad\xff",4)});
 Check(QueryLibrary(malformed,{"portal"}).size()==3,"malformed unrelated UTF-8 cannot break query");
 std::puts("library_query_test passed");
}
