#include "PX_Syntax_const_string_list.h"

PX_SYNTAX_FUNCTION(PX_Syntax_Parse_const_string_list_begin)
{
	px_abi* pnewabi;
	if (!(pnewabi=PX_Syntax_NewAbi(pSyntax,"const_string_list",pSyntax->reg_lifetime)))
	{
		PX_Syntax_Terminate(pSyntax, "out of memory!");
		return PX_FALSE;
	}
	if(!PX_AbiSet_int(pnewabi, "list_count", 0))
	{
		PX_Syntax_Terminate(pSyntax, "out of memory!");
		return PX_FALSE;
	}

	if (!PX_AbiSet_int(pnewabi, "begin", PX_Syntax_GetCurrentLexemeBegin(pSyntax)))
	{
		PX_Syntax_Terminate(pSyntax, "out of memory!");
		return PX_FALSE;
	}
	return PX_TRUE;
}

PX_SYNTAX_FUNCTION(PX_Syntax_Parse_const_string_list_new)
{
	px_char index_named[16] = { 0 };
	px_abi* plastabi = PX_Syntax_GetAbiLast(pSyntax);
	px_abi* psecondlastabi = PX_Syntax_GetAbiSecondLast(pSyntax);
	const px_char* pname;
	const px_int* plistcount;
	px_int listcount;
	pname = PX_AbiGet_string(plastabi, "name");
	if (!PX_strequ(pname, "const_string"))
	{
		return PX_FALSE;
	}

	pname = PX_AbiGet_string(psecondlastabi, "name");
	PX_ASSERTIFX(!pname || !PX_strequ(pname, "const_string_list"), "unexpected logic!");
	plistcount = PX_AbiGet_int(psecondlastabi, "list_count");
	PX_ASSERTIFX(!plistcount, "unexpected logic!");
	listcount = *plistcount;
	PX_AbiSet_int(psecondlastabi, "list_count", listcount + 1);
	PX_itoa(listcount, index_named, sizeof(index_named), 10);
	if (!PX_AbiSet_Abi(psecondlastabi, index_named, plastabi))
	{
		PX_Syntax_Terminate(pSyntax, "out of memory!");
		return PX_FALSE;
	}

	if (!PX_AbiSet_int(plastabi, "end", PX_Syntax_GetCurrentLexemeEnd(pSyntax)))
	{
		PX_Syntax_Terminate(pSyntax, "out of memory!");
		return PX_FALSE;
	}
	PX_Syntax_PopAbi(pSyntax);
	return PX_TRUE;
}

PX_SYNTAX_FUNCTION(PX_Syntax_Parse_const_string_list_end)
{
	return PX_TRUE;
}

px_bool PX_Syntax_load_const_string_list(PX_Syntax* pSyntax)
{
	PX_Syntax_Parse_PEBNF(pSyntax, "const_string_list", PX_Syntax_Parse_const_string_list_begin);
	PX_Syntax_Parse_PEBNF(pSyntax, "const_string_list = const_string", PX_Syntax_Parse_const_string_list_new);
	PX_Syntax_Parse_PEBNF(pSyntax, "const_string_list = const_string ',' ...", 0);
	PX_Syntax_Parse_PEBNF(pSyntax, "const_string_list = const_string ',' *", PX_Syntax_Parse_const_string_list_end);
	PX_Syntax_Parse_PEBNF(pSyntax, "const_string_list = const_string *", PX_Syntax_Parse_const_string_list_end);
	
	return PX_TRUE;
}