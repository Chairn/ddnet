/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_CLIENT_COMPONENTS_CONTROLS_H
#define GAME_CLIENT_COMPONENTS_CONTROLS_H

#include <base/vmath.h>

#include <engine/client.h>

#include <game/client/component.h>
#include <game/generated/protocol.h>

class CControls : public CComponent
{
	float GetMaxMouseDistance() const;

public:
	vec2 m_aMousePos[NUM_DUMMIES];
	vec2 m_aTargetPos[NUM_DUMMIES];
	float m_OldMouseX = 0;
	float m_OldMouseY = 0;

	int m_aAmmoCount[NUM_WEAPONS] = {0};

	CNetObj_PlayerInput m_aInputData[NUM_DUMMIES];
	CNetObj_PlayerInput m_aLastData[NUM_DUMMIES];
	int m_aInputDirectionLeft[NUM_DUMMIES] = {0};
	int m_aInputDirectionRight[NUM_DUMMIES] = {0};
	int m_aShowHookColl[NUM_DUMMIES] = {0};
	int m_LastDummy = 0;
	int m_OtherFire = 0;

	CControls();
	virtual int Sizeof() const override { return sizeof(*this); }

	virtual void OnReset() override;
	virtual void OnRelease() override;
	virtual void OnRender() override;
	virtual void OnMessage(int MsgType, void *pRawMsg) override;
	virtual bool OnCursorMove(float x, float y, IInput::ECursorType CursorType) override;
	virtual void OnConsoleInit() override;
	virtual void OnPlayerDeath();

	int SnapInput(int *pData);
	void ClampMousePos();
	void ResetInput(int Dummy);
};
#endif
