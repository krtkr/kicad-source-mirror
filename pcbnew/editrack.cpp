/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2012 Jean-Pierre Charras, jean-pierre.charras@ujf-grenoble.fr
 * Copyright (C) 2012 SoftPLC Corporation, Dick Hollenbeck <dick@softplc.com>
 * Copyright (C) 2012 Wayne Stambaugh <stambaughw@verizon.net>
 * Copyright (C) 1992-2012 KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you may find one here:
 * http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
 * or you may search the http://www.gnu.org website for the version 2 license,
 * or you may write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 */

/**
 * @file editrack.cpp
 */

#include <fctsys.h>
#include <class_drawpanel.h>
#include <trigo.h>
#include <pcb_edit_frame.h>

#include <pcbnew.h>
#include <drc.h>
#include <protos.h>

#include <class_board.h>
#include <class_track.h>
#include <class_zone.h>
#include <connectivity_data.h>


static void Abort_Create_Track( EDA_DRAW_PANEL* panel, wxDC* DC );
void        ShowNewTrackWhenMovingCursor( EDA_DRAW_PANEL* aPanel, wxDC* aDC,
                                          const wxPoint& aPosition, bool aErase );
static void ComputeBreakPoint( TRACK* track, int n, wxPoint end );
static void DeleteNullTrackSegments( BOARD* pcb, DLIST<TRACK>& aTrackList );
static void EnsureEndTrackOnPad( D_PAD* Pad );

// A PICKED_ITEMS_LIST to store tracks  which are modified/added/deleted
// during a track editing:
static PICKED_ITEMS_LIST s_ItemsListPicker;


/* Function called to abort a track creation
 */
static void Abort_Create_Track( EDA_DRAW_PANEL* Panel, wxDC* DC )
{
    PCB_EDIT_FRAME* frame = (PCB_EDIT_FRAME*) Panel->GetParent();
    BOARD* pcb = frame->GetBoard();
    TRACK* track = dyn_cast<TRACK*>( frame->GetCurItem() );

    if( track )
    {
        // Erase the current drawing
        ShowNewTrackWhenMovingCursor( Panel, DC, wxDefaultPosition, false );

        if( pcb->IsHighLightNetON() )
            frame->HighLight( DC );

        pcb->PopHighLight();

        if( pcb->IsHighLightNetON() )
            pcb->DrawHighLight( Panel, DC, pcb->GetHighLightNetCode() );

        frame->ClearMsgPanel();

        // Undo pending changes (mainly a lock point creation) and clear the
        // undo picker list:
        frame->PutDataInPreviousState( &s_ItemsListPicker, false, false );
        s_ItemsListPicker.ClearListAndDeleteItems();

        // Delete current (new) track
        g_CurrentTrackList.DeleteAll();
    }

    frame->SetCurItem( NULL );
}

/*
 * This function starts a new track segment.
 * If a new track segment is in progress, ends this current new segment,
 * and created a new one.
 */
TRACK* PCB_EDIT_FRAME::Begin_Route( TRACK* aTrack, wxDC* aDC )
{
    TRACK*  trackOnStartPoint = NULL;
    LSET    layerMask( GetScreen()->m_Active_Layer );
    wxPoint pos = GetCrossHairPosition();

    BOARD_CONNECTED_ITEM* lockPoint;

    if( aTrack == NULL )  // Starting a new track segment
    {
        m_canvas->SetMouseCapture( ShowNewTrackWhenMovingCursor, Abort_Create_Track );

        // Prepare the undo command info
        s_ItemsListPicker.ClearListAndDeleteItems();  // Should not be necessary, but...

        GetBoard()->PushHighLight();

        // erase old highlight
        if( GetBoard()->IsHighLightNetON() )
            HighLight( aDC );

        g_CurrentTrackList.PushBack( new TRACK( GetBoard() ) );
        g_CurrentTrackSegment->SetFlags( IS_NEW );

        GetBoard()->SetHighLightNet( 0 );

        // Search for a starting point of the new track, a track or pad
        lockPoint = GetBoard()->GetLockPoint( pos, layerMask );

        D_PAD* pad = NULL;
        if( lockPoint ) // An item (pad or track) is found
        {
            if( lockPoint->Type() == PCB_PAD_T )
            {
                pad = (D_PAD*) lockPoint;

                // A pad is found: put the starting point on pad center
                pos = pad->GetPosition();
                GetBoard()->SetHighLightNet( pad->GetNetCode() );
            }
            else // A track segment is found
            {
                trackOnStartPoint    = (TRACK*) lockPoint;
                GetBoard()->SetHighLightNet( trackOnStartPoint->GetNetCode() );
                GetBoard()->CreateLockPoint( pos, trackOnStartPoint, &s_ItemsListPicker );
            }
        }
        else
        {
            // Not a starting point, but a filled zone area can exist. This is also a
            // good starting point.
            ZONE_CONTAINER* zone;
            zone = GetBoard()->HitTestForAnyFilledArea( pos,
                                                        GetScreen()-> m_Active_Layer,
                                                        GetScreen()-> m_Active_Layer,
                                                        -1 );

            if( zone )
                GetBoard()->SetHighLightNet( zone->GetNetCode() );
        }

        DBG( g_CurrentTrackList.VerifyListIntegrity() );

        int net = -1;

        if( lockPoint )
            net = lockPoint->GetNetCode();

        BuildAirWiresTargetsList( lockPoint, wxPoint( 0, 0 ), net );

        DBG( g_CurrentTrackList.VerifyListIntegrity() );

        GetBoard()->HighLightON();
        GetBoard()->DrawHighLight( m_canvas, aDC, GetBoard()->GetHighLightNetCode() );

        // Display info about track Net class, and init track and vias sizes:
        g_CurrentTrackSegment->SetNetCode( GetBoard()->GetHighLightNetCode() );
        SetCurrentNetClass( g_CurrentTrackSegment->GetNetClassName() );

        g_CurrentTrackSegment->SetLayer( GetScreen()->m_Active_Layer );
        g_CurrentTrackSegment->SetWidth( GetDesignSettings().GetCurrentTrackWidth() );

        if( GetDesignSettings().m_UseConnectedTrackWidth )
        {
            if( trackOnStartPoint && trackOnStartPoint->Type() == PCB_TRACE_T )
                g_CurrentTrackSegment->SetWidth( trackOnStartPoint->GetWidth());
        }

        g_CurrentTrackSegment->SetStart( pos );
        g_CurrentTrackSegment->SetEnd( pos );

        if( pad )
        {
            // Useful to display track length, if the pad has a die length:
            g_CurrentTrackSegment->SetState( BEGIN_ONPAD, true );
            g_CurrentTrackSegment->start = pad;
        }

        if( Settings().m_legacyUseTwoSegmentTracks )
        {
            // Create 2nd segment
            g_CurrentTrackList.PushBack( (TRACK*)g_CurrentTrackSegment->Clone() );

            DBG( g_CurrentTrackList.VerifyListIntegrity(); );

            g_CurrentTrackSegment->start = g_FirstTrackSegment;
            g_FirstTrackSegment->end     = g_CurrentTrackSegment;

            g_FirstTrackSegment->SetState( BEGIN_ONPAD | END_ONPAD, false );
        }

        DBG( g_CurrentTrackList.VerifyListIntegrity(); );

        SetMsgPanel( g_CurrentTrackSegment );
        SetCurItem( g_CurrentTrackSegment, false );
        m_canvas->CallMouseCapture( aDC, wxDefaultPosition, false );

        if( Settings().m_legacyDrcOn )
        {
            if( BAD_DRC == m_drc->DrcOnCreatingTrack( g_CurrentTrackSegment, GetBoard()->m_Track ) )
            {
                return g_CurrentTrackSegment;
            }
        }
    }
    else   // Track in progress : segment coordinates are updated by ShowNewTrackWhenMovingCursor.
    {
        // Test for a D.R.C. error:
        if( Settings().m_legacyDrcOn )
        {
            if( BAD_DRC == m_drc->DrcOnCreatingTrack( g_CurrentTrackSegment, GetBoard()->m_Track ) )
                return NULL;

            // We must handle 2 segments
            if( Settings().m_legacyUseTwoSegmentTracks && g_CurrentTrackSegment->Back() )
            {
                if( BAD_DRC == m_drc->DrcOnCreatingTrack( g_CurrentTrackSegment->Back(),
                                                          GetBoard()->m_Track ) )
                    return NULL;
            }
        }

        /* Current track is Ok: current segment is kept, and a new one is
         * created unless the current segment is null, or 2 last are null
         * if this is a 2 segments track build.
         */
        bool CanCreateNewSegment = true;

        if( !Settings().m_legacyUseTwoSegmentTracks && g_CurrentTrackSegment->IsNull() )
            CanCreateNewSegment = false;

        if( Settings().m_legacyUseTwoSegmentTracks && g_CurrentTrackSegment->IsNull()
          && g_CurrentTrackSegment->Back()
          && g_CurrentTrackSegment->Back()->IsNull() )
            CanCreateNewSegment = false;

        if( CanCreateNewSegment )
        {
            // Erase old track on screen
            DBG( g_CurrentTrackList.VerifyListIntegrity(); );

            ShowNewTrackWhenMovingCursor( m_canvas, aDC, wxDefaultPosition, false );

            DBG( g_CurrentTrackList.VerifyListIntegrity(); );

            if( g_Raccord_45_Auto )
                Add45DegreeSegment( aDC );

            TRACK* previousTrack = g_CurrentTrackSegment;

            TRACK* newTrack = (TRACK*)g_CurrentTrackSegment->Clone();
            g_CurrentTrackList.PushBack( newTrack );
            newTrack->SetFlags( IS_NEW );

            newTrack->SetState( BEGIN_ONPAD | END_ONPAD, false );
            newTrack->start = previousTrack->end;

            DBG( g_CurrentTrackList.VerifyListIntegrity(); );

            newTrack->SetStart( newTrack->GetEnd() );

            newTrack->SetLayer( GetScreen()->m_Active_Layer );

            if( !GetDesignSettings().m_UseConnectedTrackWidth )
                newTrack->SetWidth( GetDesignSettings().GetCurrentTrackWidth() );

            DBG( g_CurrentTrackList.VerifyListIntegrity(); );

            // Show the new position
            ShowNewTrackWhenMovingCursor( m_canvas, aDC, wxDefaultPosition, false );
        }
    }

    SetCurItem( g_CurrentTrackSegment, false );
    return g_CurrentTrackSegment;
}


bool PCB_EDIT_FRAME::Add45DegreeSegment( wxDC* aDC )
{
    int dx0, dy0, dx1, dy1;

    if( g_CurrentTrackList.GetCount() < 2 )
        return false;                         // There must be 2 segments.

    TRACK* curTrack  = g_CurrentTrackSegment;
    TRACK* prevTrack = curTrack->Back();

    // Test if we have 2 consecutive track segments ( not via ) to connect.
    if( curTrack->Type() != PCB_TRACE_T || prevTrack->Type() != PCB_TRACE_T )
    {
        return false;
    }

    int segm_step_45 = KiROUND( GetScreen()->GetGridSize().x / 2 );

    if( segm_step_45 < ( curTrack->GetWidth() * 2 ) )
        segm_step_45 = curTrack->GetWidth() * 2;

    // Test if the segments are horizontal or vertical.
    dx0 = prevTrack->GetEnd().x - prevTrack->GetStart().x;
    dy0 = prevTrack->GetEnd().y - prevTrack->GetStart().y;

    dx1 = curTrack->GetEnd().x - curTrack->GetStart().x;
    dy1 = curTrack->GetEnd().y - curTrack->GetStart().y;

    // Segments should have a min length.
    if( std::max( abs( dx0 ), abs( dy0 ) ) < ( segm_step_45 * 2 ) )
        return false;

    if( std::max( abs( dx1 ), abs( dy1 ) ) < ( segm_step_45 * 2 ) )
        return false;

    // Create a new segment and connect it with the previous 2 segments.
    TRACK* newTrack = (TRACK*)curTrack->Clone();

    newTrack->SetStart( prevTrack->GetEnd() );
    newTrack->SetEnd( curTrack->GetStart() );

    if( dx0 == 0 )          // Previous segment is Vertical
    {
        if( dy1 != 0 )      // 2 segments are not 90 degrees.
        {
            delete newTrack;
            return false;
        }

        /* Calculate coordinates the connection point.
         * The new segment connects the 1st vertical segment and the 2nd
         * horizontal segment.
         */
        if( dy0 > 0 )
            newTrack->SetStart( wxPoint(newTrack->GetStart().x, newTrack->GetStart().y -segm_step_45) );
        else
            newTrack->SetStart( wxPoint(newTrack->GetStart().x, newTrack->GetStart().y + segm_step_45) );

        if( dx1 > 0 )
            newTrack->SetEnd( wxPoint(newTrack->GetEnd().x + segm_step_45, newTrack->GetEnd().y) );
        else
            newTrack->SetEnd( wxPoint(newTrack->GetEnd().x - segm_step_45, newTrack->GetEnd().y) );

        if( Settings().m_legacyDrcOn &&
            BAD_DRC == m_drc->DrcOnCreatingTrack( curTrack, GetBoard()->m_Track ) )
        {
            delete newTrack;
            return false;
        }

        prevTrack->SetEnd( newTrack->GetStart());
        curTrack->SetStart( newTrack->GetEnd());

        g_CurrentTrackList.Insert( newTrack, curTrack );
        return true;
    }

    if( dy0 == 0 )      // Previous segment is horizontal
    {
        if( dx1 != 0 )  // 2 segments are not 90 degrees
        {
            delete newTrack;
            return false;
        }

        /*  Calculate the coordinates of the point of connection:
         * A new segment has been created, connecting segment 1
         * (horizontal) and segment 2 (vertical)
         */
        if( dx0 > 0 )
            newTrack->SetStart( wxPoint(newTrack->GetStart().x - segm_step_45 , newTrack->GetStart().y));
        else
            newTrack->SetStart( wxPoint(newTrack->GetStart().x + segm_step_45, newTrack->GetStart().y) );

        if( dy1 > 0 )
            newTrack->SetEnd( wxPoint(newTrack->GetEnd().x, newTrack->GetEnd().y + segm_step_45) );
        else
            newTrack->SetEnd( wxPoint(newTrack->GetEnd().x, newTrack->GetEnd().y - segm_step_45) );

        if( Settings().m_legacyDrcOn &&
            BAD_DRC == m_drc->DrcOnCreatingTrack( newTrack, GetBoard()->m_Track ) )
        {
            delete newTrack;
            return false;
        }

        prevTrack->SetEnd( newTrack->GetStart());
        curTrack->SetStart( newTrack->GetEnd());

        g_CurrentTrackList.Insert( newTrack, curTrack );
        return true;
    }

    return false;
}


bool PCB_EDIT_FRAME::End_Route( TRACK* aTrack, wxDC* aDC )
{
    LSET layerMask( GetScreen()->m_Active_Layer );

    if( aTrack == NULL )
        return false;

    if( Settings().m_legacyDrcOn &&
        BAD_DRC == m_drc->DrcOnCreatingTrack( g_CurrentTrackSegment, GetBoard()->m_Track ) )
        return false;

    // Saving the coordinate of end point of the trace
    wxPoint pos = g_CurrentTrackSegment->GetEnd();

    DBG( g_CurrentTrackList.VerifyListIntegrity(); );

    if( Begin_Route( aTrack, aDC ) == NULL )
        return false;

    // Update last track position
    ShowNewTrackWhenMovingCursor( m_canvas, aDC, wxDefaultPosition, true );
    // Erase the last drawings
    ShowNewTrackWhenMovingCursor( m_canvas, aDC, wxDefaultPosition, false );

    DBG( g_CurrentTrackList.VerifyListIntegrity(); );

    /* The track here is now chained to the list of track segments.
     * It must be seen in the area of net
     * As close as possible to the segment base (or end), because
     * This helps to reduce the computing time */

    // Attaching the end point of the new track to a pad or a track
    BOARD_CONNECTED_ITEM* lockPoint = GetBoard()->GetLockPoint( pos, layerMask );

    if( lockPoint )
    {
        if( lockPoint->Type() ==  PCB_PAD_T )     // End of track is on a pad.
        {
            EnsureEndTrackOnPad( (D_PAD*) lockPoint );
        }
        else        // If end point of is on a different track,
                    // creates a lock point if not exists
        {
             // Creates a lock point, if not already exists:
            wxPoint hp = g_CurrentTrackSegment->GetEnd();
            lockPoint = GetBoard()->CreateLockPoint( hp, (TRACK*) lockPoint, &s_ItemsListPicker );
            g_CurrentTrackSegment->SetEnd(hp);
        }
    }

    // Delete null length segments:
    DeleteNullTrackSegments( GetBoard(), g_CurrentTrackList );

    // Insert new segments if they exist.
    // g_FirstTrackSegment can be NULL on a double click on the starting point
    if( g_FirstTrackSegment != NULL )
    {
        int    netcode    = g_FirstTrackSegment->GetNetCode();
        TRACK* firstTrack = g_FirstTrackSegment;
        int    newCount   = 0;

        // Put entire new current segment list in BOARD, and prepare undo command
        TRACK* track;
        TRACK* insertBeforeMe = g_CurrentTrackSegment->GetBestInsertPoint( GetBoard() );

        while( ( track = g_CurrentTrackList.PopFront() ) != nullptr )
        {
            ITEM_PICKER picker( track, UR_NEW );
            s_ItemsListPicker.PushItem( picker );
            GetBoard()->m_Track.Insert( track, insertBeforeMe );
            GetBoard()->GetConnectivity()->Add( track );
            track->ClearFlags();
            track->SetState( BUSY, false );
            newCount++;
        }

        // delete the old track, if it exists and is redundant
        if( Settings().m_legacyAutoDeleteOldTrack )
        {
            EraseRedundantTrack( aDC, firstTrack, newCount, &s_ItemsListPicker );
        }

        SaveCopyInUndoList( s_ItemsListPicker, UR_UNSPECIFIED );
        s_ItemsListPicker.ClearItemsList(); // s_ItemsListPicker is no more owner of picked items

        // Erase old ratsnest
        if( GetBoard()->IsElementVisible( LAYER_RATSNEST ) && aDC )
        {
            GRSetDrawMode( aDC, GR_XOR );
            DrawGeneralRatsnest( aDC, 0 );
        }

        // compute and display the new ratsnest
        TestNetConnection( aDC, netcode );
        OnModify();
        SetMsgPanel( GetBoard() );

        // Redraw the entire new track.
        DrawTraces( m_canvas, aDC, firstTrack, newCount, GR_OR );
    }

    wxASSERT( g_FirstTrackSegment == NULL );
    wxASSERT( g_CurrentTrackSegment == NULL );
    wxASSERT( g_CurrentTrackList.GetCount() == 0 );

    if( GetBoard()->IsHighLightNetON() )
        HighLight( aDC );

    GetBoard()->PopHighLight();

    if( GetBoard()->IsHighLightNetON() )
        GetBoard()->DrawHighLight( m_canvas, aDC, GetBoard()->GetHighLightNetCode() );

    m_canvas->SetMouseCapture( NULL, NULL );
    SetCurItem( NULL );

    return true;
}


TRACK* LocateIntrusion( TRACK* listStart, TRACK* aTrack, LAYER_NUM aLayer, const wxPoint& aRef )
{
    int     net   = aTrack->GetNetCode();
    int     width = aTrack->GetWidth();

    TRACK*  found = NULL;

    for( TRACK* track = listStart; track; track = track->Next() )
    {
        if( track->Type() == PCB_TRACE_T )    // skip vias
        {
            if( track->GetState( BUSY | IS_DELETED ) )
                continue;

            if( aLayer != track->GetLayer() )
                continue;

            if( track->GetNetCode() == net )
                continue;

            // TRACK::HitTest
            int dist = (width + track->GetWidth()) / 2 + aTrack->GetClearance( track );

            if( !TestSegmentHit( aRef, track->GetStart(), track->GetEnd(), dist ) )
                continue;

            found = track;

            // prefer intrusions from the side, not the end
            wxPoint pos = aRef - track->GetStart();
            wxPoint vec = track->GetEnd() - track->GetStart();
            double tmp = (double) pos.x * vec.x + (double) pos.y * vec.y;

            if( tmp >= 0 && tmp <= (double) vec.x * vec.x + (double) vec.y * vec.y )
                break;
        }
    }

    return found;
}


/**
 * Function PushTrack
 * detects if the mouse is pointing into a conflicting track.
 * In this case, it tries to push the new track out of the conflicting track's
 * clearance zone. This gives us a cheap mechanism for drawing tracks that
 * tightly follow others, independent of grid settings.
 *
 * KNOWN BUGS:
 * - we do the same sort of search and calculation up to three times:
 *   1) we search for magnetic hits (in controle.cpp)
 *   2) we check if there's a DRC violation in the making (also controle.cpp)
 *   3) we try to fix the DRC violation (here)
 * - if we have a magnetic hit and a DRC violation at the same time, we choose
 *   the magnetic hit instead of solving the violation
 * - should locate conflicting tracks also when we're crossing over them
 */
static void PushTrack( EDA_DRAW_PANEL* panel )
{
    PCB_SCREEN* screen = (PCB_SCREEN*) panel->GetParent()->GetScreen();
    BOARD*      pcb    = ( (PCB_BASE_FRAME*) (panel->GetParent()) )->GetBoard();
    wxPoint     cursor = panel->GetParent()->GetCrossHairPosition();
    wxPoint     cv, vec, n;
    TRACK*      track = g_CurrentTrackSegment;
    TRACK*      other;
    double      det;
    int         dist;
    double      f;

    other = LocateIntrusion( pcb->m_Track, track, screen->m_Active_Layer, panel->GetParent()->RefPos( true ) );

    // are we currently pointing into a conflicting trace ?
    if( !other )
        return;

    if( other->GetNetCode() == track->GetNetCode() )
        return;

    cv  = cursor - other->GetStart();
    vec = other->GetEnd() - other->GetStart();

    det = (double) cv.x * vec.y - (double) cv.y * vec.x;

    // cursor is right at the center of the old track
    if( !det )
        return;

    dist = (track->GetWidth() + 1) / 2 + (other->GetWidth() + 1) / 2 + track->GetClearance( other ) + 2;

    /*
     * DRC wants >, so +1.
     * We may have a quantization error of 1/sqrt(2), so +1 again.
     */

    // Vector "n" is perpendicular to "other", pointing towards the cursor.
    if( det > 0 )
    {
        n.x = vec.y;
        n.y = -vec.x;
    }
    else
    {
        n.x = -vec.y;
        n.y = vec.x;
    }

    f   = dist / hypot( double(n.x), double(n.y) );
    n.x = KiROUND( f * n.x );
    n.y = KiROUND( f * n.y );

    wxPoint hp = track->GetEnd();
    FindBestGridPointOnTrack( &hp, cursor, other );
    track->SetEnd( hp + n );
}


//Helper function: Draws Via circle and Via Clearance circle.
inline void DrawViaCirclesWhenEditingNewTrack( EDA_RECT* aPanelClipBox,
                                               wxDC* aDC, const wxPoint& aPos,
                                               int aViaRadius,
                                               int aViaRadiusWithClearence,
                                               COLOR4D aColor)
{
    //Current viasize clearance circle
    GRCircle( aPanelClipBox, aDC, aPos.x, aPos.y, aViaRadiusWithClearence, aColor );
    //Current viasize circle
    GRCircle( aPanelClipBox, aDC, aPos.x, aPos.y, aViaRadius, aColor );
}

/* Redraw the current track being created when the mouse cursor is moved
 */
void ShowNewTrackWhenMovingCursor( EDA_DRAW_PANEL* aPanel, wxDC* aDC, const wxPoint& aPosition,
                                   bool aErase )
{
//    DBG( g_CurrentTrackList.VerifyListIntegrity(); );

    PCB_SCREEN*     screen = (PCB_SCREEN*) aPanel->GetScreen();
    PCB_BASE_FRAME* frame  = (PCB_BASE_FRAME*) aPanel->GetParent();
    auto displ_opts = (PCB_DISPLAY_OPTIONS*) aPanel-> GetDisplayOptions();

    bool tmp = displ_opts->m_DisplayPcbTrackFill;
    displ_opts->m_DisplayPcbTrackFill = true;
    auto showTrackClearanceMode = displ_opts->m_ShowTrackClearanceMode;

    if ( g_FirstTrackSegment == NULL )
        return;

    NETCLASSPTR netclass = g_FirstTrackSegment->GetNetClass();

    if( showTrackClearanceMode != PCB_DISPLAY_OPTIONS::DO_NOT_SHOW_CLEARANCE )
        displ_opts->m_ShowTrackClearanceMode = PCB_DISPLAY_OPTIONS::SHOW_CLEARANCE_ALWAYS;

    // Values to Via circle
    int boardViaRadius = frame->GetDesignSettings().GetCurrentViaSize()/2;
    int viaRadiusWithClearence = boardViaRadius+netclass->GetClearance();
    EDA_RECT* panelClipBox=aPanel->GetClipBox();

#ifndef USE_WX_OVERLAY
    // Erase old track
    if( aErase )
    {
        DrawTraces( aPanel, aDC, g_FirstTrackSegment, g_CurrentTrackList.GetCount(), GR_XOR );

        frame->TraceAirWiresToTargets( aDC );

        if( showTrackClearanceMode >= PCB_DISPLAY_OPTIONS::SHOW_CLEARANCE_NEW_TRACKS_AND_VIA_AREAS )
        {
            COLOR4D color = frame->Settings().Colors().GetLayerColor( g_CurrentTrackSegment->GetLayer() );
            DrawViaCirclesWhenEditingNewTrack( panelClipBox, aDC, g_CurrentTrackSegment->GetEnd(),
                                               boardViaRadius, viaRadiusWithClearence, color);
        }
    }
#endif
    // MacOSX seems to need this.
    if( g_CurrentTrackList.GetCount() == 0 )
        return;

    // Set track parameters, that can be modified while creating the track
    g_CurrentTrackSegment->SetLayer( screen->m_Active_Layer );

    if( !frame->GetDesignSettings().m_UseConnectedTrackWidth )
        g_CurrentTrackSegment->SetWidth( frame->GetDesignSettings().GetCurrentTrackWidth() );

    if( frame->Settings().m_legacyUseTwoSegmentTracks )
    {
        TRACK* previous_track = g_CurrentTrackSegment->Back();

        if( previous_track  &&  previous_track->Type()==PCB_TRACE_T )
        {
            previous_track->SetLayer( screen->m_Active_Layer );

            if( !frame->GetDesignSettings().m_UseConnectedTrackWidth )
                previous_track->SetWidth( frame->GetDesignSettings().GetCurrentTrackWidth() );
        }
    }

    if( frame->Settings().m_legacyUse45DegreeTracks )
    {
        if( frame->Settings().m_legacyUseTwoSegmentTracks )
        {
            g_CurrentTrackSegment->SetEnd( frame->GetCrossHairPosition() );

            if( frame->Settings().m_legacyDrcOn )
                PushTrack( aPanel );

            ComputeBreakPoint( g_CurrentTrackSegment,
                               g_CurrentTrackList.GetCount(),
                               g_CurrentTrackSegment->GetEnd() );
        }
        else
        {
            /* Calculate of the end of the path for the permitted directions:
             * horizontal, vertical or 45 degrees.
             */
            wxPoint hp = g_CurrentTrackSegment->GetEnd();
            hp = CalculateSegmentEndPoint( frame->GetCrossHairPosition(),
                                           g_CurrentTrackSegment->GetStart() );
            g_CurrentTrackSegment->SetEnd(hp);
        }
    }
    else    // Here the angle is arbitrary
    {
        g_CurrentTrackSegment->SetEnd( frame->GetCrossHairPosition() );
    }

    // Redraw the new track
    DBG( g_CurrentTrackList.VerifyListIntegrity(); );
    DrawTraces( aPanel, aDC, g_FirstTrackSegment, g_CurrentTrackList.GetCount(), GR_XOR );

    if( showTrackClearanceMode >= PCB_DISPLAY_OPTIONS::SHOW_CLEARANCE_NEW_TRACKS_AND_VIA_AREAS )
    {
        COLOR4D color = frame->Settings().Colors().GetLayerColor(g_CurrentTrackSegment->GetLayer());

        //Via diameter must have taken what we are using, rather than netclass value.
        DrawViaCirclesWhenEditingNewTrack( panelClipBox, aDC, g_CurrentTrackSegment->GetEnd(),
                                           boardViaRadius, viaRadiusWithClearence, color);

    }

    /* Display info about current segment and the full new track:
     *  Choose the interesting segment: because we are using a 2 segments step,
     *  the last segment can be null, and the previous segment can be the
     *  interesting segment.
     */
    TRACK* isegm = g_CurrentTrackSegment;

    if( isegm->GetLength() == 0 && g_CurrentTrackSegment->Back() )
        isegm = g_CurrentTrackSegment->Back();

    // display track info:
    frame->SetMsgPanel( isegm );
    wxString msg;

    // Display current segments count (number of segments in this new track):
    msg.Printf( "%d", g_CurrentTrackList.GetCount() );
    frame->AppendMsgPanel( _( "Segs Count" ), msg, DARKCYAN );

    displ_opts->m_ShowTrackClearanceMode = showTrackClearanceMode;
    displ_opts->m_DisplayPcbTrackFill    = tmp;

    frame->BuildAirWiresTargetsList( NULL, g_CurrentTrackSegment->GetEnd(),
                                     g_CurrentTrackSegment->GetNetCode() );
    frame->TraceAirWiresToTargets( aDC );
}


wxPoint CalculateSegmentEndPoint( const wxPoint& aPosition, const wxPoint& aOrigin )
{
    // Determine end point for a segment direction 0, 90, or 45 degrees
    // depending on it's position from the origin \a aOrigin and \a aPosition.
    wxPoint endPoint;

    int deltax = aPosition.x - aOrigin.x;
    int deltay = aPosition.y - aOrigin.y;

    deltax = abs( deltax );
    deltay = abs( deltay );
    int angle  = 45;

    if( deltax >= deltay )
    {
        if( deltax == 0 )
            angle = 0;
        else if( ( (deltay << 6 ) / deltax ) < 26 )
            angle = 0;
    }
    else
    {
        angle = 45;

        if( deltay == 0 )
            angle = 90;
        else if( ( (deltax << 6 ) / deltay ) < 26 )
            angle = 90;
    }

    switch( angle )
    {
    case 0:
        endPoint.x = aPosition.x;
        endPoint.y = aOrigin.y;
        break;

    case 45:
        deltax = std::min( deltax, deltay );
        deltay = deltax;

        // Recalculate the signs for deltax and deltaY.
        if( ( aPosition.x - aOrigin.x ) < 0 )
            deltax = -deltax;

        if( ( aPosition.y - aOrigin.y ) < 0 )
            deltay = -deltay;

        endPoint.x = aOrigin.x + deltax;
        endPoint.y = aOrigin.y + deltay;
        break;

    case 90:
        endPoint.x = aOrigin.x;
        endPoint.y = aPosition.y;
        break;
    }

    return endPoint;
}


/**
 * Compute new track angle based on previous track.
 */
void ComputeBreakPoint( TRACK* track, int SegmentCount, wxPoint end )
{
    int iDx    = 0;
    int iDy    = 0;
    int iAngle = 0;

    if( SegmentCount <= 0 )
        return;

    if( track == NULL )
        return;

    TRACK* newTrack = track;
    track = track->Back();
    SegmentCount--;

    if( track )
    {
        iDx = end.x - track->GetStart().x;
        iDy = end.y - track->GetStart().y;

        iDx = abs( iDx );
        iDy = abs( iDy );
    }

    TRACK* lastTrack = track ? track->Back() : NULL;

    if( lastTrack )
    {
        if(( (lastTrack->GetEnd().x == lastTrack->GetStart().x)
           || (lastTrack->GetEnd().y == lastTrack->GetStart().y) )
        && !g_Alternate_Track_Posture)
        {
            iAngle = 45;
        }
    }
    else
    {
        if( g_Alternate_Track_Posture )
        {
            iAngle = 45;
        }
    }

    if( iAngle == 0 )
    {
        if( iDx >= iDy )
            iAngle = 0;
        else
            iAngle = 90;
    }

    if( track == NULL )
        iAngle = -1;

    switch( iAngle )
    {
    case -1:
        break;

    case 0:
        if( ( end.x - track->GetStart().x ) < 0 )
            track->SetEnd(wxPoint( end.x + iDy, track->GetStart().y));
        else
            track->SetEnd(wxPoint( end.x - iDy, track->GetStart().y));
        break;

    case 45:
        iDx = std::min( iDx, iDy );
        iDy = iDx;

        // Recalculate the signs for deltax and deltaY.
        if( ( end.x - track->GetStart().x ) < 0 )
            iDx = -iDx;

        if( ( end.y - track->GetStart().y ) < 0 )
            iDy = -iDy;

        track->SetEnd(wxPoint(track->GetStart().x + iDx, track->GetStart().y + iDy));
        break;

    case 90:
        if( ( end.y - track->GetStart().y ) < 0 )
            track->SetEnd(wxPoint(track->GetStart().x , end.y + iDx));
        else
            track->SetEnd(wxPoint(track->GetStart().x , end.y - iDx));
        break;
    }

    if( track )
    {
        if( track->IsNull() )
            track->SetEnd( end );

        newTrack->SetStart( track->GetEnd() );
    }

    newTrack->SetEnd( end );
}


/* Delete track segments which have len = 0 after creating a new track
 *  return a pointer on the first segment (start of track list)
 */
void DeleteNullTrackSegments( BOARD* pcb, DLIST<TRACK>& aTrackList )
{
    if( aTrackList.GetCount() == 0 )
        return;

    TRACK*      track = aTrackList.GetFirst();
    TRACK*      firsttrack = track;
    TRACK*      oldtrack;

    BOARD_CONNECTED_ITEM* lockPoint = track->start;

    while( track != NULL )
    {
        oldtrack = track;
        track    = track->Next();

        if( !oldtrack->IsNull() )
        {
            continue;
        }

        // NULL segment, delete it
        if( firsttrack == oldtrack )
            firsttrack = track;

        delete aTrackList.Remove( oldtrack );
    }

    if( aTrackList.GetCount() == 0 )
        return;         // all the new track segments have been deleted

    // we must set the pointers on connected items and the connection status
    oldtrack = track = firsttrack;
    firsttrack->start = NULL;

    while( track != NULL )
    {
        oldtrack = track;
        track    = track->Next();
        oldtrack->end = track;

        if( track )
            track->start = oldtrack;

        oldtrack->SetStatus( 0 );
    }

    firsttrack->start = lockPoint;

    if( lockPoint && lockPoint->Type()==PCB_PAD_T )
        firsttrack->SetState( BEGIN_ONPAD, true );

    track = firsttrack;

    while( track != NULL )
    {
        TRACK* next_track = track->Next();
        lockPoint = pcb->GetPad( track, ENDPOINT_END );

        if( lockPoint )
        {
            track->end = lockPoint;
            track->SetState( END_ONPAD, true );

            if( next_track )
            {
                next_track->start = lockPoint;
                next_track->SetState( BEGIN_ONPAD, true );
            }
        }

        track = next_track;
    }
}


/* Ensure the end point of g_CurrentTrackSegment is on the pad "Pad"
 *  if no, create a new track segment if necessary
 *  and move current (or new) end segment on pad
 */
void EnsureEndTrackOnPad( D_PAD* aPad )
{
    if( g_CurrentTrackSegment->GetEnd() == aPad->GetPosition() ) // Ok !
    {
        g_CurrentTrackSegment->end = aPad;
        g_CurrentTrackSegment->SetState( END_ONPAD, true );
        return;
    }

    TRACK* lasttrack = g_CurrentTrackSegment;

    if( !g_CurrentTrackSegment->IsNull() )
    {
        // Must create a new segment, from track end to pad center
        g_CurrentTrackList.PushBack( (TRACK*)lasttrack->Clone() );

        lasttrack->end = g_CurrentTrackSegment;
    }

    g_CurrentTrackSegment->SetEnd( aPad->GetPosition() );
    g_CurrentTrackSegment->SetState( END_ONPAD, false );

    g_CurrentTrackSegment->end = aPad;
    g_CurrentTrackSegment->SetState( END_ONPAD, true );
}
