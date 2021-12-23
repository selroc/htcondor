/***************************************************************
 *
 * Copyright (C) 1990-2007, Condor Team, Computer Sciences Department,
 * University of Wisconsin-Madison, WI.
 * 
 * Licensed under the Apache License, Version 2.0 (the "License"); you
 * may not use this file except in compliance with the License.  You may
 * obtain a copy of the License at
 * 
 *    http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ***************************************************************/

#include "condor_common.h"
#include "condor_classad.h"
#include "condor_attributes.h"
#include "condor_config.h"
#include "user_job_policy.h"
#include "proc.h"
#include "condor_holdcodes.h"

#define PARAM_SYSTEM_PERIODIC_REMOVE "SYSTEM_PERIODIC_REMOVE"
#define PARAM_SYSTEM_PERIODIC_RELEASE "SYSTEM_PERIODIC_RELEASE"
#define PARAM_SYSTEM_PERIODIC_HOLD "SYSTEM_PERIODIC_HOLD"

#ifdef ENABLE_JOB_POLICY_LISTS

static void load_policy_list(const char * knob_base, std::vector<JobPolicyExpr> & policies)
{
	std::string knob; knob.reserve(32);
	knob = knob_base; knob += "_NAMES";

	StringList items;
	if (param_and_insert_unique_items(knob.c_str(), items)) {
		policies.reserve(items.number()+1);

		for (const char * tag = items.first(); tag; tag = items.next()) {
			if (YourStringNoCase("NAMES") == tag) continue;

			JobPolicyExpr policy(tag);
			knob = knob_base; policy.append_tag(knob);
			policy.set_from_config(knob.c_str());

			int parse_err = 0;
			policy.Expr(&parse_err);
			if (parse_err) {
				dprintf(D_ALWAYS, "WARNING: ignoring invalid %s expression : %s\n", knob.c_str(), policy.Str());
				continue;
			}
			if (policy.is_trivial()) continue;
			policies.push_back(policy);
		}
	}

	JobPolicyExpr old_policy;
	old_policy.set_from_config(knob_base);
	if ( ! old_policy.is_trivial()) {
		policies.push_back(old_policy);
	}
}

#else

/* If a job ad was pre user policy and it was determined to have exited. */
const char *old_style_exit = "OldStyleExit";

/* This will be one of the job actions defined above */
const char ATTR_USER_POLICY_ACTION [] = "UserPolicyAction"; 

/* This is one of: ATTR_PERIODIC_HOLD_CHECK, ATTR_PERIODIC_REMOVE_CHECK,
	ATTR_ON_EXIT_HOLD_CHECK, ATTR_ON_EXIT_REMOVE_CHECK, or
	old_style_exit. It allows killer output of what happened and why. And
	since it is defined in terms of other expressions, makes it easy
	to compare against. */
const char ATTR_USER_POLICY_FIRING_EXPR [] = "UserPolicyFiringExpr";

/* true or false, true if it is determined the job should be held or removed
	from the queue. If false, then the caller should put this job back into
	the idle state and undefine these attributes in the job ad:
	ATTR_ON_EXIT_CODE, ATTR_ON_EXIT_SIGNAL, and then change this attribute
	ATTR_ON_EXIT_BY_SIGNAL to false in the job ad. */
const char ATTR_TAKE_ACTION [] = "TakeAction";

/* If there was an error in determining the policy, this will be true. */
const char ATTR_USER_POLICY_ERROR [] = "UserPolicyError";

/* an "errno" of sorts as to why the error happened. */
const char ATTR_USER_ERROR_REASON [] = "ErrorReason";

/* This function determines what should be done with a job given the user
	policy specifed in the job ad. If no policy is specified, then a classad
	is returned detailing that nothing should be done because there wasn't
	a user policy. You are responsible for freeing the classad you get back
	from this function. It can be used in a periodic fashion on job ads
	and has the notion of "nothing should be done to this job" present
	in the classad result you get back. */
ClassAd* user_job_policy(ClassAd *jad)
{
	ClassAd *result;
	bool on_exit_hold = false, on_exit_remove = false;
	int cdate = 0;
	int adkind;
	
	if (jad == NULL)
	{
		EXCEPT( "Could not evaluate user policy due to job ad being NULL!" );
	}

	/* Set up the default response of do nothing. The caller should
		just check for this attribute and ATTR_USER_POLICY_ERROR and
		do nothing to the rest of the classad of ATTR_TAKE_ACTION
		is false. */
	result = new ClassAd;
	if (result == NULL)
	{
		EXCEPT("Out of memory!"); /* XXX should this be here? */
	}
	result->Assign(ATTR_TAKE_ACTION, false);
	result->Assign(ATTR_USER_POLICY_ERROR, false);

	/* figure out the ad kind and then do something with it */

	adkind = JadKind(jad);

	switch(adkind)
	{
		case USER_ERROR_NOT_JOB_AD:
			dprintf(D_ALWAYS, "user_job_policy(): I have something that "
					"doesn't appear to be a job ad! Ignoring.\n");

			result->Assign(ATTR_USER_POLICY_ERROR, true);
			result->Assign(ATTR_USER_ERROR_REASON, USER_ERROR_NOT_JOB_AD);

			return result;
			break;

		case USER_ERROR_INCONSISTANT:
			dprintf(D_ALWAYS, "user_job_policy(): Inconsistant jobad state "
								"with respect to user_policy. Detail "
								"follows:\n");
			{
				ExprTree *ph_expr = jad->LookupExpr(ATTR_PERIODIC_HOLD_CHECK);
				ExprTree *pr_expr = jad->LookupExpr(ATTR_PERIODIC_REMOVE_CHECK);
				ExprTree *pl_expr = jad->LookupExpr(ATTR_PERIODIC_RELEASE_CHECK);
				ExprTree *oeh_expr = jad->LookupExpr(ATTR_ON_EXIT_HOLD_CHECK);
				ExprTree *oer_expr = jad->LookupExpr(ATTR_ON_EXIT_REMOVE_CHECK);

				EmitExpression(D_ALWAYS, ATTR_PERIODIC_HOLD_CHECK, ph_expr);
				EmitExpression(D_ALWAYS, ATTR_PERIODIC_REMOVE_CHECK, pr_expr);
				EmitExpression(D_ALWAYS, ATTR_PERIODIC_RELEASE_CHECK, pl_expr);
				EmitExpression(D_ALWAYS, ATTR_ON_EXIT_HOLD_CHECK, oeh_expr);
				EmitExpression(D_ALWAYS, ATTR_ON_EXIT_REMOVE_CHECK, oer_expr);
			}

			result->Assign(ATTR_USER_POLICY_ERROR, true);
			result->Assign(ATTR_USER_ERROR_REASON, USER_ERROR_INCONSISTANT);

			return result;
			break;

		case KIND_OLDSTYLE:
			jad->LookupInteger(ATTR_COMPLETION_DATE, cdate);
			if (cdate > 0)
			{
				result->Assign(ATTR_TAKE_ACTION, true);
				result->Assign(ATTR_USER_POLICY_ACTION, REMOVE_JOB);
				result->Assign(ATTR_USER_POLICY_FIRING_EXPR, old_style_exit);
			}
			return result;
			break;

		case KIND_NEWSTYLE:
		{
			/*	The user_policy is checked in this
				order. The first one to succeed is the winner:

				periodic_hold
				periodic_exit
				on_exit_hold
				on_exit_remove
			*/

			UserPolicy userpolicy;
			userpolicy.Init();
			int analyze_result = userpolicy.AnalyzePolicy(*jad, PERIODIC_ONLY);

			/* should I perform a periodic hold? */
			if(analyze_result == HOLD_IN_QUEUE)
			{
				/* make a result classad explaining this and return it */

				result->Assign(ATTR_TAKE_ACTION, true);
				result->Assign(ATTR_USER_POLICY_ACTION, HOLD_JOB);
				result->Assign(ATTR_USER_POLICY_FIRING_EXPR,
					userpolicy.FiringExpression());

				return result;
			}

			/* Should I perform a periodic remove? */
			if(analyze_result == REMOVE_FROM_QUEUE)
			{
				/* make a result classad explaining this and return it */

				result->Assign(ATTR_TAKE_ACTION, true);
				result->Assign(ATTR_USER_POLICY_ACTION, REMOVE_JOB);
				result->Assign(ATTR_USER_POLICY_FIRING_EXPR,
					userpolicy.FiringExpression());

				return result;
			}

			/* Should I perform a periodic release? */
			if(analyze_result == RELEASE_FROM_HOLD)
			{
				/* make a result classad explaining this and return it */

				result->Assign(ATTR_TAKE_ACTION, true);
				result->Assign(ATTR_USER_POLICY_ACTION, REMOVE_JOB);
				result->Assign(ATTR_USER_POLICY_FIRING_EXPR,
					userpolicy.FiringExpression());

				return result;
			}

			/* Check to see if ExitSignal or ExitCode
				are defined, if not, then assume the
				job hadn't exited and don't check the
				policy. This could hide a mistake of
				the caller to insert those attributes
				correctly but allows checking of the
				job ad in a periodic context. */
			if (jad->LookupExpr(ATTR_ON_EXIT_CODE) == 0 &&
				jad->LookupExpr(ATTR_ON_EXIT_SIGNAL) == 0)
			{
				return result;
			}

			/* Should I hold on exit? */
			jad->LookupBool(ATTR_ON_EXIT_HOLD_CHECK, on_exit_hold);
			if (on_exit_hold)
			{
				/* make a result classad explaining this and return it */

				result->Assign(ATTR_TAKE_ACTION, true);
				result->Assign(ATTR_USER_POLICY_ACTION, HOLD_JOB);
				result->Assign(ATTR_USER_POLICY_FIRING_EXPR,
					ATTR_ON_EXIT_HOLD_CHECK);

				return result;
			}

			/* Should I remove on exit? */
			jad->LookupBool(ATTR_ON_EXIT_REMOVE_CHECK, on_exit_remove);
			if (on_exit_remove)
			{
				/* make a result classad explaining this and return it */

				result->Assign(ATTR_TAKE_ACTION, true);
				result->Assign(ATTR_USER_POLICY_ACTION, REMOVE_JOB);
				result->Assign(ATTR_USER_POLICY_FIRING_EXPR,
					ATTR_ON_EXIT_REMOVE_CHECK);

				return result;
			}

			/* just return the default of leaving the job in idle state */
			return result;

			break;
		}

		default:
			dprintf(D_ALWAYS, "JadKind() returned unknown ad kind\n");

			/* just return the default of leaving the job in idle state. This
				is safest. */
			return result;

			break;
	}

	/* just return the default of leaving the job in idle state */
	return result;
}

void EmitExpression(unsigned int mode, const char *attr, ExprTree* attr_expr)
{
	if (attr_expr == NULL)
	{
		dprintf(mode, "%s = UNDEFINED\n", attr);
	}
	else
	{
		dprintf(mode, "%s = %s\n", attr, ExprTreeToString(attr_expr));
	}
}


/* is this classad oldstyle, newstyle, or even a job ad? */
int JadKind(ClassAd *suspect)
{
	int cdate;

	/* determine if I have a user job ad with the new user policy expressions
		enabled. */
	ExprTree *ph_expr = suspect->LookupExpr(ATTR_PERIODIC_HOLD_CHECK);
	ExprTree *pr_expr = suspect->LookupExpr(ATTR_PERIODIC_REMOVE_CHECK);
	ExprTree *pl_expr = suspect->LookupExpr(ATTR_PERIODIC_RELEASE_CHECK);
	ExprTree *oeh_expr = suspect->LookupExpr(ATTR_ON_EXIT_HOLD_CHECK);
	ExprTree *oer_expr = suspect->LookupExpr(ATTR_ON_EXIT_REMOVE_CHECK);

	/* check to see if it is oldstyle */
	if (ph_expr == NULL && pr_expr == NULL && pl_expr == NULL && oeh_expr == NULL && 
		oer_expr == NULL)
	{
		/* check to see if it has ATTR_COMPLETION_DATE, if so then it is
			an oldstyle jobad. If not, it isn't a job ad at all. */

		if (suspect->LookupInteger(ATTR_COMPLETION_DATE, cdate) == 1)
		{
			return KIND_OLDSTYLE;
		}

		return USER_ERROR_NOT_JOB_AD;
	}

	/* check to see if it is a consistant user policy job ad. */
	if (ph_expr == NULL || pr_expr == NULL || pl_expr == NULL || oeh_expr == NULL || 
		oer_expr == NULL)
	{
		return USER_ERROR_INCONSISTANT;
	}
	
	return KIND_NEWSTYLE;
}

#endif // ENABLE_JOB_POLICY_LISTS

/* NEW INTERFACE */

  #define POLICY_NONE                    SYS_POLICY_NONE
  #define POLICY_SYSTEM_PERIODIC_HOLD    SYS_POLICY_PERIODIC_HOLD
  #define POLICY_SYSTEM_PERIODIC_RELEASE SYS_POLICY_PERIODIC_RELEASE
  #define POLICY_SYSTEM_PERIODIC_REMOVE  SYS_POLICY_PERIODIC_REMOVE


void UserPolicy::Init()
{
	ResetTriggers();
	Config();
}

void UserPolicy::Config()
{
	ClearConfig();

#ifdef ENABLE_JOB_POLICY_LISTS
	load_policy_list(PARAM_SYSTEM_PERIODIC_HOLD, m_sys_periodic_holds);
	load_policy_list(PARAM_SYSTEM_PERIODIC_RELEASE, m_sys_periodic_releases);
	load_policy_list(PARAM_SYSTEM_PERIODIC_REMOVE, m_sys_periodic_removes);
#else
	auto_free_ptr expr_string(param(PARAM_SYSTEM_PERIODIC_HOLD));
	if (expr_string) {
		ParseClassAdRvalExpr(expr_string, m_sys_periodic_hold);
		long long ival = 1;
		if (m_sys_periodic_hold && ExprTreeIsLiteralNumber(m_sys_periodic_hold, ival) &&  ! ival) {
			delete m_sys_periodic_hold; m_sys_periodic_hold = NULL;
		}
	}

	expr_string.set(param(PARAM_SYSTEM_PERIODIC_RELEASE));
	if (expr_string) {
		ParseClassAdRvalExpr(expr_string, m_sys_periodic_release);
		long long ival = 1;
		if (m_sys_periodic_release && ExprTreeIsLiteralNumber(m_sys_periodic_release, ival) &&  ! ival) {
			delete m_sys_periodic_release; m_sys_periodic_release = NULL;
		}
	}

	expr_string.set(param(PARAM_SYSTEM_PERIODIC_REMOVE));
	if (expr_string) {
		ParseClassAdRvalExpr(expr_string, m_sys_periodic_remove);
		long long ival = 1;
		if (m_sys_periodic_remove && ExprTreeIsLiteralNumber(m_sys_periodic_remove, ival) &&  ! ival) {
			delete m_sys_periodic_remove; m_sys_periodic_remove = NULL;
		}
	}
#endif

}

void UserPolicy::ResetTriggers()
{
	m_fire_expr_val = -1;
	m_fire_source = FS_NotYet;
	m_fire_expr = NULL;
}

int
UserPolicy::AnalyzePolicy(ClassAd & ad, int mode)
{

	int timer_remove;
	int state;

	if (mode != PERIODIC_ONLY && mode != PERIODIC_THEN_EXIT)
	{
		EXCEPT("UserPolicy Error: Unknown mode in AnalyzePolicy()");
	}

	if( ! ad.LookupInteger(ATTR_JOB_STATUS,state) ) {
		return UNDEFINED_EVAL;
	}

		// Clear out our stateful variables
	m_fire_expr = NULL;
	m_fire_expr_val = -1;
	m_fire_unparsed_expr.clear();

	/*	The user_policy is checked in this
			order. The first one to succeed is the winner:

			ATTR_ALLOWED_JOB_DURATION
			ATTR_TIMER_REMOVE_CHECK
			ATTR_PERIODIC_HOLD_CHECK
			ATTR_PERIODIC_RELEASE_CHECK
			ATTR_PERIODIC_REMOVE_CHECK
			ATTR_ON_EXIT_HOLD_CHECK
			ATTR_ON_EXIT_REMOVE_CHECK
	*/

	/* Should I perform a hold based on the "running" time of the job? */
	int allowedJobDuration;
	if( ad.LookupInteger( ATTR_JOB_ALLOWED_JOB_DURATION, allowedJobDuration ) ) {
		// Arguably, we should be calling BaseUserPolicy::getJobBirthday()
		// here, but we don't have access to that here.  This will probably
		// cause some confusion in the local universe, because it otherwise
		// uses ATTR_JOB_START_DATE to determine duration, but using
		// ATTR_SHADOW_BIRTHDATE was the assignment and is simpler.
		int birthday;
		if( ad.LookupInteger( ATTR_SHADOW_BIRTHDATE, birthday ) ) {
			if( time(NULL) - birthday >= allowedJobDuration ) {
				m_fire_expr = ATTR_JOB_ALLOWED_JOB_DURATION;
				m_fire_source = FS_JobDuration;
				formatstr(m_fire_reason, "The job exceeded allowed job duration of %d", allowedJobDuration);
				return HOLD_IN_QUEUE;
			}
		}
	}

	/* Should I perform a hold based on the "execute" time of the job? */
	if (state == RUNNING) {
		int allowedExecuteDuration, beganExecuting;
		if (ad.LookupInteger(ATTR_JOB_ALLOWED_EXECUTE_DURATION, allowedExecuteDuration) &&
			ad.LookupInteger(ATTR_JOB_CURRENT_START_EXECUTING_DATE, beganExecuting)) {

			// We use TransferOutFinished because the shadow only sets
			// ATTR_JOB_CURRENT_FINISH_TRANSFER_OUTPUT_DATE at job exit.
			int TransferOutFinished;
			bool tof = ad.LookupInteger("TransferOutFinished", TransferOutFinished);
			bool checkpointed = tof && (TransferOutFinished > beganExecuting);
			if (checkpointed) {
				beganExecuting = TransferOutFinished;
			}

			if ((time(NULL) - beganExecuting) > allowedExecuteDuration) {
				m_fire_expr = ATTR_JOB_ALLOWED_EXECUTE_DURATION;
				m_fire_source = FS_ExecuteDuration;
				formatstr(m_fire_reason, "The job exceeded allowed execute duration of %d", allowedExecuteDuration);
				return HOLD_IN_QUEUE;
			}
		}
	}

	/* Should I perform a remove based on the epoch time? */
	m_fire_expr = ATTR_TIMER_REMOVE_CHECK;
	if ( ! ad.LookupInteger(ATTR_TIMER_REMOVE_CHECK, timer_remove)) {
		//check if attribute exists, but is undefined
		ExprTree * expr = ad.Lookup(ATTR_TIMER_REMOVE_CHECK);
		if (expr != NULL)
		{
			m_fire_expr_val = -1;
			m_fire_source = FS_JobAttribute;
			ExprTreeToString(expr, m_fire_unparsed_expr);
			return UNDEFINED_EVAL;
		}
		timer_remove = -1;
	}
	if( timer_remove >= 0 && timer_remove < time(NULL) ) {
		m_fire_expr_val = 1;
		m_fire_source = FS_JobAttribute;
		ExprTreeToString(ad.Lookup(ATTR_TIMER_REMOVE_CHECK), m_fire_unparsed_expr);
		return REMOVE_FROM_QUEUE;
	}

	int retval;

	/* should I perform a periodic hold? */
	if(state!=HELD) {
		if(AnalyzeSinglePeriodicPolicy(ad, ATTR_PERIODIC_HOLD_CHECK, POLICY_SYSTEM_PERIODIC_HOLD, HOLD_IN_QUEUE, retval)) {
			return retval;
		}
	} else {
	/* Should I perform a periodic release? */
		if(AnalyzeSinglePeriodicPolicy(ad, ATTR_PERIODIC_RELEASE_CHECK, POLICY_SYSTEM_PERIODIC_RELEASE, RELEASE_FROM_HOLD, retval)) {
			return retval;
		}
	}

	/* Should I perform a periodic remove? */
	if(AnalyzeSinglePeriodicPolicy(ad, ATTR_PERIODIC_REMOVE_CHECK, POLICY_SYSTEM_PERIODIC_REMOVE, REMOVE_FROM_QUEUE, retval)) {
		return retval;
	}

	if( mode == PERIODIC_ONLY ) {
			// Nothing left to do, just return the default
		m_fire_expr = NULL;
		return STAYS_IN_QUEUE;
	}

	/* else it is PERIODIC_THEN_EXIT so keep going */

	/* This better be in the classad because it determines how the process
		exited, either by signal, or by exit() */
	if( ! ad.LookupExpr(ATTR_ON_EXIT_BY_SIGNAL) ) {
		EXCEPT( "UserPolicy Error: %s is not present in the classad",
				ATTR_ON_EXIT_BY_SIGNAL );
	}

	/* Check to see if ExitSignal or ExitCode
		are defined, if not, then except because
		caller should have filled this in if calling
		this function saying to check the exit policies. */
	if( ad.LookupExpr(ATTR_ON_EXIT_CODE) == 0 &&
		ad.LookupExpr(ATTR_ON_EXIT_SIGNAL) == 0 )
	{
		EXCEPT( "UserPolicy Error: No signal/exit codes in job ad!" );
	}

	/* Should I hold on exit? */
	if (AnalyzeSinglePeriodicPolicy(ad, ATTR_ON_EXIT_HOLD_CHECK, POLICY_NONE, HOLD_IN_QUEUE, retval)) {
		return retval;
	}

	/* Should I remove on exit? */
	m_fire_expr = ATTR_ON_EXIT_REMOVE_CHECK;
	m_fire_source = FS_JobAttribute;
	m_fire_reason.clear();
	m_fire_subcode = 0;
	ExprTree * expr = ad.Lookup(ATTR_ON_EXIT_REMOVE_CHECK);
	if (expr) {
		classad::Value val;
		if (ad.EvaluateExpr(expr, val) && val.IsNumber(m_fire_expr_val) && m_fire_expr_val == 0) {
			// for backward compatibility, unparse the trigger expression for use in writing
			// the log terminate event.
			ExprTreeToString(expr, m_fire_unparsed_expr);

			// OnExitRemove was false, which means we want the job to stay in the queue...
			return STAYS_IN_QUEUE;
		}
	}

	// no expression, or evaluated to anything but false - remove
	m_fire_expr_val = 1;
	return REMOVE_FROM_QUEUE;
}

bool UserPolicy::AnalyzeSinglePeriodicPolicy(ClassAd & ad, ExprTree * expr, int on_true_return, int & retval)
{
	ASSERT(expr);

	int result = 0;
	long long ival = 0;

	classad::Value val;
	if (ad.EvaluateExpr(expr, val) && val.IsNumber(ival)) {
		result = (ival != 0);
	} else {
#if 0 // we don't want to treat undefined as triggering 
		if (ExprTreeIsLiteral(expr, val) && val.IsUndefinedValue()) {
			// if the expr is defined to be undefined, treat that a false result.
			result = 0;
		} else {
			// if the expr evaluates to undefined, treat that as a triggering expression.
			m_fire_expr_val = -1;
			retval = UNDEFINED_EVAL;
			return true;
		}
#endif
	}

	if( result ) {
		m_fire_expr_val = 1;
		retval = on_true_return;
		return true;
	}

	return false;
}

bool UserPolicy::AnalyzeSinglePeriodicPolicy(ClassAd & ad, const char * attrname, SysPolicyId sys_policy, int on_true_return, int & retval)
{
	ASSERT(attrname);

	// Evaluate the specified expression in the job ad
	m_fire_expr = attrname;
	ExprTree * expr = ad.Lookup(attrname);
	if (expr && AnalyzeSinglePeriodicPolicy(ad, expr, on_true_return, retval)) {
		m_fire_source = FS_JobAttribute;
		m_fire_reason.clear();
		m_fire_subcode = 0;

		// Save expression, subcode and reason for the FiringReason() method.
		ExprTreeToString(expr, m_fire_unparsed_expr);
		if (m_fire_expr_val != -1) {
			std::string attr(attrname); attr += "SubCode";
			ad.EvaluateAttrNumber(attr, m_fire_subcode);
			attr = m_fire_expr; attr += "Reason";
			ad.EvaluateAttrString(attr, m_fire_reason);
		}
		return true;
	}

#ifdef ENABLE_JOB_POLICY_LISTS // multi policy
	const char * policy_name = NULL;
	std::vector<JobPolicyExpr> * policies = nullptr;
	switch (sys_policy) {
	case POLICY_SYSTEM_PERIODIC_HOLD:
		policies = &m_sys_periodic_holds;
		policy_name = PARAM_SYSTEM_PERIODIC_HOLD;
		break;
	case POLICY_SYSTEM_PERIODIC_RELEASE:
		policies = &m_sys_periodic_releases;
		policy_name = PARAM_SYSTEM_PERIODIC_RELEASE;
		break;
	case POLICY_SYSTEM_PERIODIC_REMOVE:
		policies = &m_sys_periodic_removes;
		policy_name = PARAM_SYSTEM_PERIODIC_REMOVE;
		break;
	default:
		return false;
	}
	for (auto & policy : *policies) {
		// TODO: remove this line once all above are lists
		expr = policy.Expr();
		if (! expr) continue;
#else
	const char * policy_name = NULL;
	switch (sys_policy) {
	case POLICY_SYSTEM_PERIODIC_HOLD:
		expr = m_sys_periodic_hold;
		policy_name = PARAM_SYSTEM_PERIODIC_HOLD;
		break;
	case POLICY_SYSTEM_PERIODIC_RELEASE:
		expr = m_sys_periodic_release;
		policy_name = PARAM_SYSTEM_PERIODIC_RELEASE;
		break;
	case POLICY_SYSTEM_PERIODIC_REMOVE:
		expr = m_sys_periodic_remove;
		policy_name = PARAM_SYSTEM_PERIODIC_REMOVE;
		break;
	default:
		expr = NULL;
		break;
	}

	if (expr) {
#endif
		long long ival = 0;
		classad::Value val;
		if (ad.EvaluateExpr(expr, val) && val.IsNumber(ival) && ival != 0) {
			m_fire_expr_val = 1;
			m_fire_expr = policy_name;
			m_fire_source = FS_SystemMacro;
			m_fire_reason.clear();
			m_fire_subcode = 0;

			retval = on_true_return;

		#ifdef ENABLE_JOB_POLICY_LISTS // multi policy
			m_fire_unparsed_expr = policy.Str();
		#else
			// fetch the unparsed value of the expression that fired.
			ExprTreeToString(expr, m_fire_unparsed_expr);
		#endif

			// temp buffer for building _SUBCODE and _REASON param names.
			std::string param_sub;

			std::string expr_string;
			param_sub = policy_name;
		#ifdef ENABLE_JOB_POLICY_LISTS
			policy.append_tag(param_sub);
		#endif
			param_sub += "_SUBCODE";
			if (param(expr_string, param_sub.c_str(), "") && ! expr_string.empty()) {
				long long ival;
				classad::Value val;
				if (ad.EvaluateExpr(expr_string, val) && val.IsNumber(ival)) {
					m_fire_subcode = (int)ival;
				}
			}

			param_sub = policy_name;
		#ifdef ENABLE_JOB_POLICY_LISTS
			policy.append_tag(param_sub);
		#endif
			param_sub += "_REASON";
			if (param(expr_string, param_sub.c_str(), "") && ! expr_string.empty()) {
				classad::Value val;
				if (ad.EvaluateExpr(expr_string, val) && val.IsStringValue(m_fire_reason)) {
					// val.IsStringValue will have already set m_fire_reason
				}
			}

			return true;
		}
	}

	return false;
}

const char* UserPolicy::FiringExpression(void)
{
	return m_fire_expr;
}

bool UserPolicy::FiringReason(std::string &reason,int &reason_code,int &reason_subcode)
{
	reason_code = 0;
	reason_subcode = 0;

	if ( m_fire_expr == NULL ) {
		return false;
	}

	reason = "";

	const char * expr_src;
	std::string exprString;
	switch(m_fire_source) {
		case FS_NotYet:
			expr_src = "UNKNOWN (never set)";
			break;

		case FS_JobAttribute:
			expr_src = "job attribute";
			exprString = m_fire_unparsed_expr.c_str();
			if (m_fire_expr_val == -1) {
				reason_code = CONDOR_HOLD_CODE::JobPolicyUndefined;
			} else {
				reason_code = CONDOR_HOLD_CODE::JobPolicy;
				reason_subcode = m_fire_subcode;
				reason = m_fire_reason;
			}
			break;

		case FS_JobDuration:
			reason = m_fire_reason;
			reason_code = CONDOR_HOLD_CODE::JobDurationExceeded;
			reason_subcode = 0;
			break;

		case FS_ExecuteDuration:
			reason = m_fire_reason;
			reason_code = CONDOR_HOLD_CODE::JobExecuteExceeded;
			reason_subcode = 0;
			break;

		case FS_SystemMacro:
			expr_src = "system macro";
			exprString = m_fire_unparsed_expr.c_str();
			if( m_fire_expr_val == -1 ) {
				reason_code = CONDOR_HOLD_CODE::SystemPolicyUndefined;
			}
			else {
				reason_code = CONDOR_HOLD_CODE::SystemPolicy;
				reason_subcode = m_fire_subcode;
				reason = m_fire_reason;
			}
			break;

		default:
			expr_src = "UNKNOWN (bad value)";
			break;
	}

	if( !reason.empty() ) {
		return true;
	}

	// Format up the reason string
	formatstr( reason, "The %s %s expression '%s' evaluated to ",
					expr_src,
					m_fire_expr,
					exprString.c_str());

	// Get a string for it's value
	switch( m_fire_expr_val ) {
	case 0:
		reason += "FALSE";
		break;
	case 1:
		reason += "TRUE";
		break;
	case -1:
		reason += "UNDEFINED";
		break;
	default:
		EXCEPT( "Unrecognized FiringExpressionValue: %d", 
				m_fire_expr_val ); 
		break;
	}

	return true;
}
